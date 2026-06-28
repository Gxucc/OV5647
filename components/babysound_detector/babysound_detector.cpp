/**
 * @file babysound_detector.cpp
 */

#include "babysound_detector.h"
#include <cmath>
#include <cstring>

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "dl_mfcc.hpp"
#include "dl_speech_features.hpp"

#include "esp_log.h"
#include "esp_timer.h"

// ==================== 配置 ====================

static const char* TAG = BABYSOUND_TAG;

// 默认归一化参数（来自训练集 norm.npz）
static constexpr float DEFAULT_MFCC_MEAN = -1.455099f;
static constexpr float DEFAULT_MFCC_STD  = 3.909205f;

// 量化参数
static constexpr int INPUT_EXPONENT  = -4;
static constexpr int OUTPUT_EXPONENT = -7;

// MFCC 配置
static constexpr int SAMPLE_RATE     = 16000;
static constexpr int FRAME_LENGTH  = 32;
static constexpr int FRAME_SHIFT   = 16;
static constexpr int NUM_MEL_BINS  = 40;
static constexpr int NUM_CEPS      = 13;
static constexpr float PREEMPHASIS = 0.97f;
static constexpr float LOW_FREQ    = 0.0f;
static constexpr float HIGH_FREQ   = 8000.0f;

// 模型输入尺寸
static constexpr int NUM_FRAMES = 61;
static constexpr int MFCC_DIM  = 12;

// 音频缓冲区
static constexpr int PCM_SAMPLES = 16000;

// ==================== 全局变量 ====================

static dl::Model *s_model = nullptr;
static dl::TensorBase *s_input_tensor = nullptr;
static dl::TensorBase *s_output_tensor = nullptr;
static dl::audio::MFCC *s_mfcc = nullptr;
static bool s_initialized = false;

// 工作缓冲区
static float s_mfcc_raw[65 * 13];
static float s_mfcc_12dim[61 * 12];
static int8_t s_quantized_input[61 * 12];

// ==================== 内部函数 ====================

static esp_err_t init_mfcc_extractor(void)
{
    dl::audio::SpeechFeatureConfig config;
    config.sample_rate = SAMPLE_RATE;
    config.frame_length = FRAME_LENGTH;
    config.frame_shift = FRAME_SHIFT;
    config.num_mel_bins = NUM_MEL_BINS;
    config.num_ceps = NUM_CEPS;
    config.preemphasis = PREEMPHASIS;
    config.cepstral_lifter = 0.0f;
    config.window_type = dl::audio::WinType::HAMMING;
    config.low_freq = LOW_FREQ;
    config.high_freq = HIGH_FREQ;
    config.use_energy = false;
    config.raw_energy = false;
    config.remove_dc_offset = true;
    config.log_epsilon = 1.1920928955078125e-07f;
    config.use_log_fbank = 1;

    s_mfcc = new dl::audio::MFCC(config);
    if (!s_mfcc) {
        ESP_LOGE(TAG, "Failed to create MFCC extractor");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "MFCC extractor initialized");
    return ESP_OK;
}

static esp_err_t extract_mfcc_12dim(const int16_t *pcm_buffer, float *out_mfcc)
{
    if (!s_mfcc) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = s_mfcc->process(pcm_buffer, PCM_SAMPLES, s_mfcc_raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MFCC process failed: %s", esp_err_to_name(ret));
        return ret;
    }

    auto shape = s_mfcc->get_output_shape(PCM_SAMPLES);
    int num_frames = shape[0];
    int num_ceps = shape[1];

    if (num_ceps != 13) {
        ESP_LOGE(TAG, "Unexpected MFCC dims: %d", num_ceps);
        return ESP_ERR_INVALID_STATE;
    }

    int frames_to_use = (num_frames > NUM_FRAMES) ? NUM_FRAMES : num_frames;

    for (int i = 0; i < NUM_FRAMES; i++) {
        int src_frame = (i < frames_to_use) ? i : (frames_to_use - 1);

        for (int j = 0; j < MFCC_DIM; j++) {
            out_mfcc[i * MFCC_DIM + j] = s_mfcc_raw[src_frame * 13 + (j + 1)];
        }
    }

    return ESP_OK;
}

static esp_err_t preprocess_mfcc(const int16_t *pcm_buffer)
{
    // 1. 提取 MFCC（12维）
    esp_err_t ret = extract_mfcc_12dim(pcm_buffer, s_mfcc_12dim);
    if (ret != ESP_OK) {
        return ret;
    }

    // 2. 归一化 + 量化
    float quant_scale = std::pow(2.0f, (float)(-INPUT_EXPONENT));

    for (int i = 0; i < NUM_FRAMES * MFCC_DIM; i++) {
        float normalized = (s_mfcc_12dim[i] - DEFAULT_MFCC_MEAN) / DEFAULT_MFCC_STD;
        float q = normalized * quant_scale;

        if (q > 127.0f) q = 127.0f;
        if (q < -128.0f) q = -128.0f;

        s_quantized_input[i] = (int8_t)std::round(q);
    }

    // 3. 复制到模型输入 tensor
    int8_t *input_ptr = (int8_t *)s_input_tensor->get_element_ptr();
    memcpy(input_ptr, s_quantized_input, NUM_FRAMES * MFCC_DIM * sizeof(int8_t));

    // 调试日志
    int min_q = 127, max_q = -128;
    float sum_q = 0.0f;
    for (int i = 0; i < NUM_FRAMES * MFCC_DIM; i++) {
        if (s_quantized_input[i] < min_q) min_q = s_quantized_input[i];
        if (s_quantized_input[i] > max_q) max_q = s_quantized_input[i];
        sum_q += s_quantized_input[i];
    }
    ESP_LOGD(TAG, "Quantized input: min=%d, max=%d, mean=%.2f", 
             min_q, max_q, sum_q / (NUM_FRAMES * MFCC_DIM));

    return ESP_OK;
}

static esp_err_t postprocess_softmax(babysound_result_t *result)
{
    if (!s_output_tensor || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    int8_t *output_data = (int8_t *)s_output_tensor->get_element_ptr();
    int num_outputs = s_output_tensor->get_size();

    if (num_outputs < BABYSOUND_NUM_CLASSES) {
        ESP_LOGE(TAG, "Output size mismatch: %d", num_outputs);
        return ESP_ERR_INVALID_STATE;
    }

    // 反量化
    float dequant_scale = std::pow(2.0f, (float)OUTPUT_EXPONENT);
    float logit_non_baby = (float)output_data[0] * dequant_scale;
    float logit_baby = (float)output_data[1] * dequant_scale;

    // Softmax
    float max_val = std::max(logit_non_baby, logit_baby);
    float exp_non_baby = std::exp(logit_non_baby - max_val);
    float exp_baby = std::exp(logit_baby - max_val);
    float sum = exp_non_baby + exp_baby;

    result->non_baby_prob = exp_non_baby / sum;
    result->baby_prob = exp_baby / sum;

    if (result->baby_prob > result->non_baby_prob) {
        result->predicted_class = CLASS_BABY;
        result->label = "BABY";
    } else {
        result->predicted_class = CLASS_NON_BABY;
        result->label = "NON_BABY";
    }

    return ESP_OK;
}

// ==================== 公共 API ====================

extern "C" esp_err_t babysound_detector_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing babysound detector...");

    // 1. 加载模型
    s_model = new dl::Model(
        "model_audio",
        fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
        0,
        dl::MEMORY_MANAGER_GREEDY,
        nullptr,
        true
    );

    if (!s_model || s_model->get_fbs_model() == nullptr) {
        ESP_LOGE(TAG, "Model load failed");
        if (s_model) {
            delete s_model;
            s_model = nullptr;
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Model loaded successfully");

    // 2. 获取输入输出 tensor
    s_input_tensor = s_model->get_input();
    s_output_tensor = s_model->get_output();

    if (!s_input_tensor || !s_output_tensor) {
        ESP_LOGE(TAG, "Failed to get tensors");
        delete s_model;
        s_model = nullptr;
        return ESP_FAIL;
    }

    // 3. 验证输入形状
    auto input_shape = s_input_tensor->get_shape();
    ESP_LOGI(TAG, "Input shape: [%d, %d, %d, %d]",
             input_shape[0], input_shape[1], input_shape[2], input_shape[3]);
    ESP_LOGI(TAG, "Input dtype: %s",
             s_input_tensor->get_dtype() == dl::DATA_TYPE_INT8 ? "INT8" : "OTHER");
    ESP_LOGI(TAG, "Input exponent: %d", s_input_tensor->get_exponent());

    // 4. 验证输出形状
    auto output_shape = s_output_tensor->get_shape();
    ESP_LOGI(TAG, "Output shape: [%d, %d]",
             output_shape[0], output_shape[1]);

    // 5. 初始化 MFCC
    ESP_ERROR_CHECK(init_mfcc_extractor());

    s_initialized = true;
    ESP_LOGI(TAG, "Babysound detector initialized successfully");
    return ESP_OK;
}

extern "C" void babysound_detector_deinit(void)
{
    if (s_model) {
        delete s_model;
        s_model = nullptr;
    }
    if (s_mfcc) {
        delete s_mfcc;
        s_mfcc = nullptr;
    }
    s_input_tensor = nullptr;
    s_output_tensor = nullptr;
    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}

extern "C" esp_err_t babysound_detector_run(const int16_t *pcm_buffer, babysound_result_t *result)
{
    if (!s_initialized || !s_model || !pcm_buffer || !result) {
        ESP_LOGE(TAG, "Not initialized or invalid params");
        return ESP_ERR_INVALID_STATE;
    }

    auto start = esp_timer_get_time();

    // 1. 预处理
    esp_err_t ret = preprocess_mfcc(pcm_buffer);
    if (ret != ESP_OK) {
        return ret;
    }

    auto preprocess_end = esp_timer_get_time();

    // 2. 推理
    s_model->run();

    auto infer_end = esp_timer_get_time();

    // 3. 后处理
    ret = postprocess_softmax(result);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "%s (baby: %.3f, non-baby: %.3f) | Preprocess: %lld us, Infer: %lld us",
             result->label, result->baby_prob, result->non_baby_prob,
             preprocess_end - start, infer_end - preprocess_end);

    return ESP_OK;
}

extern "C" bool babysound_detector_is_baby_cry(const int16_t *pcm_buffer, float threshold)
{
    babysound_result_t result;
    esp_err_t ret = babysound_detector_run(pcm_buffer, &result);
    if (ret != ESP_OK) return false;
    return (result.predicted_class == CLASS_BABY && result.baby_prob > threshold);
}

extern "C" esp_err_t babysound_detector_debug_preprocess(const int16_t *pcm_buffer)
{
    if (!s_initialized || !s_mfcc || !pcm_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    // 1. 提取 MFCC
    esp_err_t ret = s_mfcc->process(pcm_buffer, PCM_SAMPLES, s_mfcc_raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MFCC process failed");
        return ret;
    }

    auto shape = s_mfcc->get_output_shape(PCM_SAMPLES);
    int num_frames = shape[0];

    int frames_to_use = (num_frames > NUM_FRAMES) ? NUM_FRAMES : num_frames;

    for (int i = 0; i < NUM_FRAMES; i++) {
        int src_frame = (i < frames_to_use) ? i : (frames_to_use - 1);
        for (int j = 0; j < MFCC_DIM; j++) {
            s_mfcc_12dim[i * MFCC_DIM + j] = s_mfcc_raw[src_frame * 13 + (j + 1)];
        }
    }

    // 2. 保存原始 MFCC
    FILE *f_raw = fopen("/sdcard/debug_mfcc_raw.txt", "w");
    if (f_raw) {
        for (int i = 0; i < NUM_FRAMES; i++) {
            for (int j = 0; j < MFCC_DIM; j++) {
                fprintf(f_raw, "%.6f", s_mfcc_12dim[i * MFCC_DIM + j]);
                if (j < MFCC_DIM - 1) fprintf(f_raw, ",");
            }
            fprintf(f_raw, "\n");
        }
        fclose(f_raw);
        ESP_LOGI(TAG, "Saved raw MFCC to /sdcard/debug_mfcc_raw.txt");
    }

    // 3. 归一化并保存
    float quant_scale = std::pow(2.0f, (float)(-INPUT_EXPONENT));
    
    FILE *f_norm = fopen("/sdcard/debug_mfcc_norm.txt", "w");
    FILE *f_quant = fopen("/sdcard/debug_mfcc_quant.txt", "w");
    
    if (f_norm && f_quant) {
        for (int i = 0; i < NUM_FRAMES; i++) {
            for (int j = 0; j < MFCC_DIM; j++) {
                float val = s_mfcc_12dim[i * MFCC_DIM + j];
                float normalized = (val - DEFAULT_MFCC_MEAN) / DEFAULT_MFCC_STD;
                float q = normalized * quant_scale;
                
                if (q > 127.0f) q = 127.0f;
                if (q < -128.0f) q = -128.0f;
                
                int8_t qi = (int8_t)std::round(q);

                fprintf(f_norm, "%.6f", normalized);
                fprintf(f_quant, "%d", (int)qi);
                
                if (j < MFCC_DIM - 1) {
                    fprintf(f_norm, ",");
                    fprintf(f_quant, ",");
                }
            }
            fprintf(f_norm, "\n");
            fprintf(f_quant, "\n");
        }
        fclose(f_norm);
        fclose(f_quant);
        ESP_LOGI(TAG, "Saved norm MFCC to /sdcard/debug_mfcc_norm.txt");
        ESP_LOGI(TAG, "Saved quant MFCC to /sdcard/debug_mfcc_quant.txt");
    }

    return ESP_OK;
}