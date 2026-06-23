/**
 * @file fall_classifier.cpp
 * @brief ESP32-P4 跌倒分类器（分区标签加载方式）
 */

#include "fall_classifier.h"
#include <cmath>
#include <algorithm>

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = FALL_CLASSIFIER_TAG;

static dl::Model *s_model = nullptr;
static bool s_initialized = false;
static dl::TensorBase* s_input_tensor = nullptr;
static dl::TensorBase* s_output_tensor = nullptr;

static const float MEAN[3] = {0.485f, 0.456f, 0.406f};
static const float STD[3]  = {0.229f, 0.224f, 0.225f};

static uint8_t s_r5_to_r8[32];
static uint8_t s_g6_to_g8[64];
static uint8_t s_b5_to_b8[32];
static bool s_tables_initialized = false;

static void init_rgb565_tables(void)
{
    if (s_tables_initialized) return;
    for (int i = 0; i < 32; i++) s_r5_to_r8[i] = (i << 3) | (i >> 2);
    for (int i = 0; i < 64; i++) s_g6_to_g8[i] = (i << 2) | (i >> 4);
    for (int i = 0; i < 32; i++) s_b5_to_b8[i] = (i << 3) | (i >> 2);
    s_tables_initialized = true;
    ESP_LOGI(TAG, "RGB565 lookup tables initialized");
}

static void preprocess_crop_resize_quantize(
    const uint16_t* src_rgb565, int src_w, int src_h,
    int box_x, int box_y, int box_w, int box_h,
    int8_t* dst_quantized, int input_exponent)
{
    if (box_x < 0) box_x = 0;
    if (box_y < 0) box_y = 0;
    if (box_x + box_w > src_w) box_w = src_w - box_x;
    if (box_y + box_h > src_h) box_h = src_h - box_y;

    if (box_w <= 0 || box_h <= 0) {
        float scale = std::pow(2.0f, (float)(-input_exponent));
        float gray_norm = (0.5f - 0.485f) / 0.229f;
        int8_t gray_quant = (int8_t)std::round(gray_norm * scale);
        gray_quant = std::max((int8_t)-128, std::min((int8_t)127, gray_quant));
        for (int i = 0; i < FALL_CLASS_INPUT_SIZE * FALL_CLASS_INPUT_SIZE * 3; i++) {
            dst_quantized[i] = gray_quant;
        }
        return;
    }

    float scale = std::min(
        (float)FALL_CLASS_INPUT_SIZE / (float)box_w,
        (float)FALL_CLASS_INPUT_SIZE / (float)box_h
    );

    int new_w = (int)(box_w * scale);
    int new_h = (int)(box_h * scale);
    int x_offset = (FALL_CLASS_INPUT_SIZE - new_w) / 2;
    int y_offset = (FALL_CLASS_INPUT_SIZE - new_h) / 2;
    float quant_scale = std::pow(2.0f, (float)(-input_exponent));

    float gray_norm = (0.5f - 0.485f) / 0.229f;
    int8_t gray_quant = (int8_t)std::round(gray_norm * quant_scale);
    gray_quant = std::max((int8_t)-128, std::min((int8_t)127, gray_quant));

    for (int i = 0; i < FALL_CLASS_INPUT_SIZE * FALL_CLASS_INPUT_SIZE * 3; i++) {
        dst_quantized[i] = gray_quant;
    }

    for (int dy = 0; dy < new_h; dy++) {
        for (int dx = 0; dx < new_w; dx++) {
            int src_x = box_x + (dx * box_w) / new_w;
            int src_y = box_y + (dy * box_h) / new_h;
            src_x = std::min(src_x, box_x + box_w - 1);
            src_y = std::min(src_y, box_y + box_h - 1);

            uint16_t rgb565 = src_rgb565[src_y * src_w + src_x];
            int dst_x = x_offset + dx;
            int dst_y = y_offset + dy;

            uint8_t r = s_r5_to_r8[(rgb565 >> 11) & 0x1F];
            uint8_t g = s_g6_to_g8[(rgb565 >> 5) & 0x3F];
            uint8_t b = s_b5_to_b8[rgb565 & 0x1F];

            float norm_r = ((r / 255.0f) - MEAN[0]) / STD[0];
            float norm_g = ((g / 255.0f) - MEAN[1]) / STD[1];
            float norm_b = ((b / 255.0f) - MEAN[2]) / STD[2];

            int8_t q_r = (int8_t)std::round(norm_r * quant_scale);
            int8_t q_g = (int8_t)std::round(norm_g * quant_scale);
            int8_t q_b = (int8_t)std::round(norm_b * quant_scale);

            q_r = std::max((int8_t)-128, std::min((int8_t)127, q_r));
            q_g = std::max((int8_t)-128, std::min((int8_t)127, q_g));
            q_b = std::max((int8_t)-128, std::min((int8_t)127, q_b));

            int base = dst_y * FALL_CLASS_INPUT_SIZE + dst_x;
            dst_quantized[0 * FALL_CLASS_INPUT_SIZE * FALL_CLASS_INPUT_SIZE + base] = q_r;
            dst_quantized[1 * FALL_CLASS_INPUT_SIZE * FALL_CLASS_INPUT_SIZE + base] = q_g;
            dst_quantized[2 * FALL_CLASS_INPUT_SIZE * FALL_CLASS_INPUT_SIZE + base] = q_b;
        }
    }
}

extern "C" bool fall_classifier_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    // 官方方式：分区标签加载
    s_model = new dl::Model(
        "model",
        fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
        0,
        dl::MEMORY_MANAGER_GREEDY,
        nullptr,
        true
    );

    if (!s_model) {
        ESP_LOGE(TAG, "Failed to create model");
        return false;
    }

    if (s_model->get_fbs_model() == nullptr) {
        ESP_LOGE(TAG, "Model load failed - FbsModel is null");
        delete s_model;
        s_model = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Model loaded successfully via partition label");

    s_input_tensor = s_model->get_input();
    s_output_tensor = s_model->get_output();

    if (!s_input_tensor || !s_output_tensor) {
        ESP_LOGE(TAG, "Failed to get input/output tensors");
        delete s_model;
        s_model = nullptr;
        return false;
    }

    auto input_shape = s_input_tensor->get_shape();
    if (input_shape.size() < 4) {
        ESP_LOGE(TAG, "Invalid input shape");
        delete s_model;
        s_model = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Model info:");
    ESP_LOGI(TAG, "  Input shape: [%d, %d, %d, %d]",
             input_shape[0], input_shape[1], input_shape[2], input_shape[3]);
    ESP_LOGI(TAG, "  Input dtype: %s",
             s_input_tensor->get_dtype() == dl::DATA_TYPE_INT8 ? "INT8" : "OTHER");
    ESP_LOGI(TAG, "  Input exponent: %d", s_input_tensor->get_exponent());

    auto output_shape = s_output_tensor->get_shape();
    ESP_LOGI(TAG, "  Output shape: [%d, %d]",
             output_shape[0], output_shape[1]);
    ESP_LOGI(TAG, "  Output dtype: %s",
             s_output_tensor->get_dtype() == dl::DATA_TYPE_INT8 ? "INT8" : "OTHER");

    init_rgb565_tables();
    s_initialized = true;
    ESP_LOGI(TAG, "Fall classifier initialized successfully");
    return true;
}

extern "C" void fall_classifier_deinit(void)
{
    if (s_model) {
        delete s_model;
        s_model = nullptr;
    }
    s_input_tensor = nullptr;
    s_output_tensor = nullptr;
    s_initialized = false;
    ESP_LOGI(TAG, "Deinitialized");
}

extern "C" bool fall_classifier_run(const uint8_t *rgb565_buf,
                                    int img_width,
                                    int img_height,
                                    const fall_classifier_box_t *box,
                                    fall_classifier_result_t *result)
{
    if (!s_initialized || !s_model || !rgb565_buf || !box || !result) {
        ESP_LOGE(TAG, "Not initialized or invalid params");
        return false;
    }

    int input_exponent = s_input_tensor->get_exponent();
    int8_t* input_data = (int8_t*)s_input_tensor->get_element_ptr();

    auto start = esp_timer_get_time();

    preprocess_crop_resize_quantize(
        (const uint16_t*)rgb565_buf, img_width, img_height,
        box->x, box->y, box->width, box->height,
        input_data, input_exponent
    );

    auto preprocess_end = esp_timer_get_time();

    s_model->run();

    auto infer_end = esp_timer_get_time();

    int output_exponent = s_output_tensor->get_exponent();
    int8_t* output_data = (int8_t*)s_output_tensor->get_element_ptr();
    int num_outputs = s_output_tensor->get_size();

    if (num_outputs < FALL_CLASS_NUM_CLASSES) {
        ESP_LOGE(TAG, "Output size mismatch: %d", num_outputs);
        return false;
    }

    // 打印原始 INT8 输出（调试）
    ESP_LOGI(TAG, "Raw INT8 output: [%d, %d], exponent: %d",
             output_data[0], output_data[1], output_exponent);

    float fall_logit = (float)output_data[0] * std::pow(2.0f, (float)output_exponent);
    float normal_logit = (float)output_data[1] * std::pow(2.0f, (float)output_exponent);

    ESP_LOGI(TAG, "Dequantized logits: [%.3f, %.3f]", fall_logit, normal_logit);

    float max_val = std::max(fall_logit, normal_logit);
    float exp_fall = std::exp(fall_logit - max_val);
    float exp_normal = std::exp(normal_logit - max_val);
    float sum = exp_fall + exp_normal;

    result->fall_prob = exp_fall / sum;
    result->normal_prob = exp_normal / sum;

    if (result->fall_prob > result->normal_prob) {
        result->predicted_class = CLASS_FALL;
        result->label = "FALL";
    } else {
        result->predicted_class = CLASS_NORMAL;
        result->label = "NORMAL";
    }

    ESP_LOGI(TAG, "%s (fall: %.3f, normal: %.3f) | Preprocess: %lld us, Infer: %lld us",
             result->label, result->fall_prob, result->normal_prob,
             preprocess_end - start, infer_end - preprocess_end);

    return true;
}

extern "C" bool fall_classifier_is_fall(const uint8_t *rgb565_buf,
                                          int img_width,
                                          int img_height,
                                          const fall_classifier_box_t *box,
                                          float threshold)
{
    fall_classifier_result_t result;
    bool ok = fall_classifier_run(rgb565_buf, img_width, img_height, box, &result);
    if (!ok) return false;
    return (result.predicted_class == CLASS_FALL && result.fall_prob > threshold);
}