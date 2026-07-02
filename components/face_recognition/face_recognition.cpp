#include "face_recognition.h"
#include "human_face_recognition.hpp"
#include "human_face_detect.hpp"
#include "dl_image.hpp"
#include "esp_log.h"

static const char *TAG = "face_recognition";

static HumanFaceRecognizer *s_recognizer = NULL;
static HumanFaceDetect *s_detector = NULL;
static float s_threshold = 0.65f;

#define COLOR_RED    0xF800
#define COLOR_GREEN  0x07E0

/* ==================== 辅助画框函数 ==================== */

static inline void draw_hline(uint16_t *buf, int x1, int x2, int y, int w, int h, uint16_t color)
{
    if (y < 0 || y >= h) return;
    if (x1 < 0) x1 = 0;
    if (x2 >= w) x2 = w - 1;
    if (x1 > x2) return;
    for (int x = x1; x <= x2; x++) {
        buf[y * w + x] = color;
    }
}

static inline void draw_vline(uint16_t *buf, int x, int y1, int y2, int w, int h, uint16_t color)
{
    if (x < 0 || x >= w) return;
    if (y1 < 0) y1 = 0;
    if (y2 >= h) y2 = h - 1;
    if (y1 > y2) return;
    for (int y = y1; y <= y2; y++) {
        buf[y * w + x] = color;
    }
}

static void draw_box(uint16_t *buf, int bw, int bh, int x, int y, int w, int h,
                     uint16_t color, int line_width)
{
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= bw) x2 = bw - 1;
    if (y2 >= bh) y2 = bh - 1;
    if (x1 >= x2 || y1 >= y2) return;

    for (int t = 0; t < line_width; t++) {
        draw_hline(buf, x1, x2, y1 + t, bw, bh, color);
        draw_hline(buf, x1, x2, y2 - t, bw, bh, color);
        draw_vline(buf, x1 + t, y1, y2, bw, bh, color);
        draw_vline(buf, x2 - t, y1, y2, bw, bh, color);
    }
}

/* ==================== C API 实现 ==================== */

extern "C" esp_err_t face_recognition_init(float threshold)
{
    if (s_recognizer || s_detector) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_threshold = threshold;

    // 创建人脸检测器
    s_detector = new HumanFaceDetect();
    if (!s_detector) {
        ESP_LOGE(TAG, "Failed to create face detector");
        return ESP_FAIL;
    }

    // 创建人脸识别器，数据库使用 flash 分区 "face_db"
    s_recognizer = new HumanFaceRecognizer("face_db");
    if (!s_recognizer) {
        delete s_detector;
        s_detector = NULL;
        ESP_LOGE(TAG, "Failed to create face recognizer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Face recognition initialized, threshold=%.2f", threshold);
    return ESP_OK;
}

extern "C" void face_recognition_deinit(void)
{
    if (s_recognizer) {
        delete s_recognizer;
        s_recognizer = NULL;
    }
    if (s_detector) {
        delete s_detector;
        s_detector = NULL;
    }
    ESP_LOGI(TAG, "Face recognition deinitialized");
}

extern "C" bool face_recognition_run(const uint8_t *rgb565_buf, int img_width, int img_height,
                                     face_recognition_result_t *out_result)
{
    if (!s_recognizer || !s_detector || !rgb565_buf || !out_result) {
        return false;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->face_id = -1;

    dl::image::img_t img = {
        .data = (void *)rgb565_buf,
        .width = (uint16_t)img_width,
        .height = (uint16_t)img_height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };

    // 1. 检测人脸
    std::list<dl::detect::result_t> detect_results = s_detector->run(img);
    if (detect_results.empty()) {
        return false;
    }

    // 取最大人脸框
    dl::detect::result_t *best = nullptr;
    int best_area = 0;
    for (auto &res : detect_results) {
        int area = res.box[2] * res.box[3];
        if (area > best_area) {
            best_area = area;
            best = &res;
        }
    }
    if (!best) {
        return false;
    }

    out_result->x = best->box[0];
    out_result->y = best->box[1];
    out_result->width = best->box[2];
    out_result->height = best->box[3];
    out_result->detect_score = best->score;

    // 2. 人脸识别
    std::vector<dl::recognition::result_t> recog_results = s_recognizer->recognize(img, detect_results);

    if (!recog_results.empty()) {
        // 取最佳匹配（按相似度排序，第一个就是最佳）
        dl::recognition::result_t &best_match = recog_results[0];
        out_result->face_id = best_match.id;
        out_result->similarity = best_match.similarity;

        if (best_match.similarity >= s_threshold) {
            out_result->recognized = true;
            ESP_LOGI(TAG, "Recognized: ID=%d, similarity=%.3f", best_match.id, best_match.similarity);
        } else {
            out_result->recognized = false;
            out_result->face_id = -1;
            ESP_LOGI(TAG, "Unknown face, best similarity=%.3f (threshold=%.2f)", best_match.similarity, s_threshold);
        }
    } else {
        out_result->recognized = false;
        ESP_LOGI(TAG, "No recognition results");
    }

    return true;
}

extern "C" bool face_recognition_process(const uint8_t *rgb565_buf, int cam_w, int cam_h,
                                         uint8_t *lcd_buf, int lcd_w, int lcd_h)
{
    if (!lcd_buf) {
        return false;
    }

    face_recognition_result_t result;
    bool detected = face_recognition_run(rgb565_buf, cam_w, cam_h, &result);

    if (detected) {
        uint16_t *pixel_buf = (uint16_t *)lcd_buf;
        uint16_t color = result.recognized ? COLOR_GREEN : COLOR_RED;
        draw_box(pixel_buf, lcd_w, lcd_h,
                 result.x, result.y, result.width, result.height,
                 color, 3);
    }

    return detected;
}