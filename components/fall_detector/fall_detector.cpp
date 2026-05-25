#include "fall_detector.h"
#include "esp_log.h"
#include "dl_model_base.hpp"
#include "dl_tool.hpp"
#include <cmath>
#include <cstring>
#include <map>

static const char *TAG = "fall_detector";

extern const uint8_t yolo11n_pose_192_espdl[] asm("_binary_yolo11n_pose_192_espdl_start");

static dl::Model *s_model = nullptr;

#define NUM_KEYPOINTS       6
#define NUM_TOTAL_KPTS      17
#define KPT_DIM             3
#define CONF_THRESHOLD      0.3f
#define MODEL_INPUT_W       192
#define MODEL_INPUT_H       192
#define NUM_ANCHORS         756

static const float PREPROCESS_MEAN[3] = {0.0f, 0.0f, 0.0f};
static const float PREPROCESS_STD[3] = {1.0f, 1.0f, 1.0f};

static const int STRIDES[3] = {8, 16, 32};
static const int GRID_SIZES[3][2] = {{24, 24}, {12, 12}, {6, 6}};

extern "C" {

int fall_detector_init(void)
{
    ESP_LOGI(TAG, "Loading YOLO11n-Pose model...");
    s_model = new dl::Model((const char*)yolo11n_pose_192_espdl,
                            "yolo11n-pose-192.espdl",
                            fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (!s_model) {
        ESP_LOGE(TAG, "Failed to create model");
        return -1;
    }
    s_model->minimize();
    
    dl::TensorBase *input = s_model->get_input();
    ESP_LOGI(TAG, "Input shape: %s", dl::vector_to_string(input->get_shape()).c_str());
    ESP_LOGI(TAG, "Input dtype: %s", input->get_dtype_string());
    
    std::map<std::string, dl::TensorBase*> outputs = s_model->get_outputs();
    int idx = 0;
    for (auto& pair : outputs) {
        ESP_LOGI(TAG, "Output[%d] name: %s, shape: %s", idx++, pair.first.c_str(),
                 dl::vector_to_string(pair.second->get_shape()).c_str());
    }
    
    ESP_LOGI(TAG, "Model loaded, %d outputs", outputs.size());
    return 0;
}

static void manual_preprocess(const uint8_t *rgb565_src, int src_w, int src_h, 
                              int8_t *dst, int dst_w, int dst_h)
{
    float scale_x = (float)src_w / dst_w;
    float scale_y = (float)src_h / dst_h;
    
    dl::TensorBase *input_tensor = s_model->get_input();
    float quant_scale = DL_RESCALE(input_tensor->get_exponent());
    
    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            int src_x = (int)(x * scale_x);
            int src_y = (int)(y * scale_y);
            int src_idx = src_y * src_w + src_x;
            
            uint16_t pixel = ((uint16_t*)rgb565_src)[src_idx];
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;
            
            int dst_idx = (y * dst_w + x) * 3;
            
            float rf = (r / 255.0f - PREPROCESS_MEAN[0]) / PREPROCESS_STD[0];
            float gf = (g / 255.0f - PREPROCESS_MEAN[1]) / PREPROCESS_STD[1];
            float bf = (b / 255.0f - PREPROCESS_MEAN[2]) / PREPROCESS_STD[2];
            
            int32_t rq = (int32_t)roundf(rf * quant_scale);
            int32_t gq = (int32_t)roundf(gf * quant_scale);
            int32_t bq = (int32_t)roundf(bf * quant_scale);
            
            dst[dst_idx + 0] = (int8_t)DL_CLIP(rq, DL_QUANT8_MIN, DL_QUANT8_MAX);
            dst[dst_idx + 1] = (int8_t)DL_CLIP(gq, DL_QUANT8_MIN, DL_QUANT8_MAX);
            dst[dst_idx + 2] = (int8_t)DL_CLIP(bq, DL_QUANT8_MIN, DL_QUANT8_MAX);
        }
    }
}

static void make_anchors(int stride, int grid_w, int grid_h, 
                         float *anchors_x, float *anchors_y)
{
    int offset = stride / 2;
    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            int idx = y * grid_w + x;
            anchors_x[idx] = x * stride + offset;
            anchors_y[idx] = y * stride + offset;
        }
    }
}

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

static void decode_scale(int8_t *box_data, int8_t *score_data, int8_t *kpt_data,
                         float box_scale, float score_scale, float kpt_scale,
                         int stride, int grid_w, int grid_h,
                         float *detections, int *num_dets, int max_dets)
{
    int num_grid = grid_w * grid_h;
    float *anchors_x = (float*)heap_caps_malloc(num_grid * sizeof(float), MALLOC_CAP_SPIRAM);
    float *anchors_y = (float*)heap_caps_malloc(num_grid * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!anchors_x || !anchors_y) {
        free(anchors_x);
        free(anchors_y);
        return;
    }
    make_anchors(stride, grid_w, grid_h, anchors_x, anchors_y);
    
    int offset = stride / 2;
    int reg_max = 16;  // YOLO11 DFL bins
    
    for (int i = 0; i < num_grid; i++) {
        if (*num_dets >= max_dets) break;
        
        float cls_score = sigmoid(dl::dequantize(score_data[i], score_scale));
        if (cls_score < CONF_THRESHOLD) continue;
        
        // 解码 box（简化版，用中心点近似）
        float dx = sigmoid(dl::dequantize(box_data[i * 64 + 0], box_scale)) * 2.0f - 0.5f;
        float dy = sigmoid(dl::dequantize(box_data[i * 64 + 1], box_scale)) * 2.0f - 0.5f;
        float dw = sigmoid(dl::dequantize(box_data[i * 64 + 2], box_scale)) * 2.0f;
        float dh = sigmoid(dl::dequantize(box_data[i * 64 + 3], box_scale)) * 2.0f;
        
        float center_x = anchors_x[i];
        float center_y = anchors_y[i];
        
        float cx = (dx + center_x / MODEL_INPUT_W) * MODEL_INPUT_W;
        float cy = (dy + center_y / MODEL_INPUT_H) * MODEL_INPUT_H;
        float w = dw * dw * stride;
        float h = dh * dh * stride;
        
        float x1 = cx - w / 2.0f;
        float y1 = cy - h / 2.0f;
        float x2 = cx + w / 2.0f;
        float y2 = cy + h / 2.0f;
        
        // 解码关键点 - 官方公式
        float kpts[NUM_TOTAL_KPTS * 3];
        for (int k = 0; k < NUM_TOTAL_KPTS; k++) {
            float kpt_x = dl::dequantize(kpt_data[i * 51 + k * 3], kpt_scale);
            float kpt_y = dl::dequantize(kpt_data[i * 51 + k * 3 + 1], kpt_scale);
            float kpt_conf = dl::dequantize(kpt_data[i * 51 + k * 3 + 2], kpt_scale);
            
            // 官方公式：kpt_x * 2.0 * stride + (center_x - offset)
            float abs_kx = kpt_x * 2.0f * stride + (center_x - offset);
            float abs_ky = kpt_y * 2.0f * stride + (center_y - offset);
            
            // 归一化到 0-1
            kpts[k * 3] = fmaxf(0, fminf(1, abs_kx / MODEL_INPUT_W));
            kpts[k * 3 + 1] = fmaxf(0, fminf(1, abs_ky / MODEL_INPUT_H));
            kpts[k * 3 + 2] = sigmoid(kpt_conf);
        }
        
        int idx = *num_dets;
        detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 0] = x1;
        detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 1] = y1;
        detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 2] = x2;
        detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 3] = y2;
        detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 4] = cls_score;
        detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 5] = 0;
        memcpy(&detections[idx * (6 + NUM_TOTAL_KPTS * 3) + 6], kpts, NUM_TOTAL_KPTS * 3 * sizeof(float));
        
        (*num_dets)++;
    }
    
    free(anchors_x);
    free(anchors_y);
}

static void nms(float *detections, int *num_dets, float iou_threshold)
{
    for (int i = 0; i < *num_dets - 1; i++) {
        for (int j = i + 1; j < *num_dets; j++) {
            if (detections[j * (6 + NUM_TOTAL_KPTS * 3) + 4] > 
                detections[i * (6 + NUM_TOTAL_KPTS * 3) + 4]) {
                float temp[6 + NUM_TOTAL_KPTS * 3];
                memcpy(temp, &detections[i * (6 + NUM_TOTAL_KPTS * 3)], sizeof(temp));
                memcpy(&detections[i * (6 + NUM_TOTAL_KPTS * 3)], 
                       &detections[j * (6 + NUM_TOTAL_KPTS * 3)], sizeof(temp));
                memcpy(&detections[j * (6 + NUM_TOTAL_KPTS * 3)], temp, sizeof(temp));
            }
        }
    }
    
    bool suppressed[100] = {false};
    for (int i = 0; i < *num_dets; i++) {
        if (suppressed[i]) continue;
        for (int j = i + 1; j < *num_dets; j++) {
            if (suppressed[j]) continue;
            
            float x1 = fmaxf(detections[i * (6 + NUM_TOTAL_KPTS * 3) + 0], 
                            detections[j * (6 + NUM_TOTAL_KPTS * 3) + 0]);
            float y1 = fmaxf(detections[i * (6 + NUM_TOTAL_KPTS * 3) + 1], 
                            detections[j * (6 + NUM_TOTAL_KPTS * 3) + 1]);
            float x2 = fminf(detections[i * (6 + NUM_TOTAL_KPTS * 3) + 2], 
                            detections[j * (6 + NUM_TOTAL_KPTS * 3) + 2]);
            float y2 = fminf(detections[i * (6 + NUM_TOTAL_KPTS * 3) + 3], 
                            detections[j * (6 + NUM_TOTAL_KPTS * 3) + 3]);
            
            float inter = fmaxf(0, x2 - x1) * fmaxf(0, y2 - y1);
            float area_i = (detections[i * (6 + NUM_TOTAL_KPTS * 3) + 2] - detections[i * (6 + NUM_TOTAL_KPTS * 3) + 0]) *
                          (detections[i * (6 + NUM_TOTAL_KPTS * 3) + 3] - detections[i * (6 + NUM_TOTAL_KPTS * 3) + 1]);
            float area_j = (detections[j * (6 + NUM_TOTAL_KPTS * 3) + 2] - detections[j * (6 + NUM_TOTAL_KPTS * 3) + 0]) *
                          (detections[j * (6 + NUM_TOTAL_KPTS * 3) + 3] - detections[j * (6 + NUM_TOTAL_KPTS * 3) + 1]);
            
            float iou = inter / (area_i + area_j - inter);
            if (iou > iou_threshold) {
                suppressed[j] = true;
            }
        }
    }
    
    int new_num = 0;
    for (int i = 0; i < *num_dets; i++) {
        if (!suppressed[i]) {
            if (new_num != i) {
                memcpy(&detections[new_num * (6 + NUM_TOTAL_KPTS * 3)], 
                       &detections[i * (6 + NUM_TOTAL_KPTS * 3)], 
                       (6 + NUM_TOTAL_KPTS * 3) * sizeof(float));
            }
            new_num++;
        }
    }
    *num_dets = new_num;
}

int fall_detector_run(const uint8_t *rgb565_buf, int src_w, int src_h, fd_result_t *out)
{
    if (!s_model || !rgb565_buf || !out) {
        return -1;
    }

    memset(out, 0, sizeof(fd_result_t));

    dl::TensorBase *input = s_model->get_input();
    int8_t *input_data = (int8_t*)input->get_element_ptr();
    
    manual_preprocess(rgb565_buf, src_w, src_h, input_data, MODEL_INPUT_W, MODEL_INPUT_H);
    
    s_model->run(input);

    std::map<std::string, dl::TensorBase*> outputs = s_model->get_outputs();
    if (outputs.size() < 9) {
        ESP_LOGE(TAG, "Expected 9 outputs, got %d", outputs.size());
        return -1;
    }

    const char* box_names[3] = {"box0", "box1", "box2"};
    const char* score_names[3] = {"score0", "score1", "score2"};
    const char* kpt_names[3] = {"kpt0", "kpt1", "kpt2"};
    
    int8_t *box_data[3] = {nullptr};
    int8_t *score_data[3] = {nullptr};
    int8_t *kpt_data[3] = {nullptr};
    float box_scale = 0, score_scale = 0, kpt_scale = 0;
    
    for (int i = 0; i < 3; i++) {
        auto box_it = outputs.find(box_names[i]);
        auto score_it = outputs.find(score_names[i]);
        auto kpt_it = outputs.find(kpt_names[i]);
        
        if (box_it == outputs.end() || score_it == outputs.end() || kpt_it == outputs.end()) {
            ESP_LOGE(TAG, "Output not found: %s/%s/%s", box_names[i], score_names[i], kpt_names[i]);
            return -1;
        }
        
        box_data[i] = (int8_t*)box_it->second->get_element_ptr();
        score_data[i] = (int8_t*)score_it->second->get_element_ptr();
        kpt_data[i] = (int8_t*)kpt_it->second->get_element_ptr();
        
        if (i == 0) {
            box_scale = DL_SCALE(box_it->second->get_exponent());
            score_scale = DL_SCALE(score_it->second->get_exponent());
            kpt_scale = DL_SCALE(kpt_it->second->get_exponent());
        }
    }

    // ========== 调试打印原始值（在 box_data 定义之后） ==========
    ESP_LOGI(TAG, "=== RAW DEBUG ===");
    ESP_LOGI(TAG, "box0[0] dx_raw=%.4f dy_raw=%.4f", 
             dl::dequantize(box_data[0][0], box_scale),
             dl::dequantize(box_data[0][1], box_scale));
    ESP_LOGI(TAG, "kpt0[11] x_raw=%.4f y_raw=%.4f v_raw=%.4f",
             dl::dequantize(kpt_data[0][11*3], kpt_scale),
             dl::dequantize(kpt_data[0][11*3+1], kpt_scale),
             dl::dequantize(kpt_data[0][11*3+2], kpt_scale));
    ESP_LOGI(TAG, "=================");

    float *detections = (float*)heap_caps_malloc(100 * (6 + NUM_TOTAL_KPTS * 3) * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!detections) {
        ESP_LOGE(TAG, "Failed to alloc detections");
        return -1;
    }

    int num_dets = 0;
    for (int i = 0; i < 3; i++) {
        decode_scale(box_data[i], score_data[i], kpt_data[i],
                     box_scale, score_scale, kpt_scale,
                     STRIDES[i], GRID_SIZES[i][0], GRID_SIZES[i][1],
                     detections, &num_dets, 100);
    }
    
    if (num_dets == 0) {
        out->valid = 0;
        free(detections);
        return 0;
    }
    
    bool *suppressed = (bool*)heap_caps_calloc(100, sizeof(bool), MALLOC_CAP_SPIRAM);
    if (!suppressed) {
        free(detections);
        return -1;
    }
    nms(detections, &num_dets, 0.5f);
    free(suppressed);
    
    if (num_dets == 0) {
        out->valid = 0;
        free(detections);
        return 0;
    }
    
    out->valid = 1;
    out->person_score = detections[4];
    
    float *best_kpts = &detections[6];
    int kpt_indices[NUM_KEYPOINTS] = {11, 12, 13, 14, 15, 16};
    
    for (int k = 0; k < NUM_KEYPOINTS; k++) {
        int idx = kpt_indices[k];
        out->kpts[k].x = best_kpts[idx * 3];
        out->kpts[k].y = best_kpts[idx * 3 + 1];
        out->kpts[k].conf = best_kpts[idx * 3 + 2];
    }

    ESP_LOGI(TAG, "Detected: score=%.3f, num_dets=%d", out->person_score, num_dets);
    for (int k = 0; k < NUM_KEYPOINTS; k++) {
        ESP_LOGI(TAG, "  Kpt%d: (%.3f, %.3f) conf=%.3f", k, out->kpts[k].x, out->kpts[k].y, out->kpts[k].conf);
    }

    free(detections);
    return 0;
}

void fall_detector_deinit(void)
{
    if (s_model) {
        delete s_model;
        s_model = nullptr;
    }
}

} // extern "C"