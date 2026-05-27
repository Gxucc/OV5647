#include "person_detect.h"
#include "pedestrian_detect.hpp"
#include "dl_image.hpp"
#include "esp_log.h"

static const char *TAG = "person_detect";
static PedestrianDetect *s_detector = NULL;

extern "C" esp_err_t person_detect_init(void)
{
    s_detector = new PedestrianDetect();
    if (!s_detector) {
        ESP_LOGE(TAG, "Failed to create detector");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Person detector initialized");
    return ESP_OK;
}

extern "C" bool person_detect_run(const uint8_t *rgb565_buf, int img_width, int img_height,
                                  person_box_t *out_box)
{
    if (!s_detector || !rgb565_buf || !out_box) {
        return false;
    }

    dl::image::img_t img = {
        .data = (void *)rgb565_buf,
        .width = (uint16_t)img_width,
        .height = (uint16_t)img_height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR565LE,  // ← BGR565LE
    };

    std::list<dl::detect::result_t> results = s_detector->run(img);
    
    ESP_LOGI(TAG, "Total detections: %d", (int)results.size());
    
    int idx = 0;
    for (auto &res : results) {
        ESP_LOGI(TAG, "  [%d] x=%d y=%d w=%d h=%d score=%.3f", 
                 idx++, res.box[0], res.box[1], res.box[2], res.box[3], res.score);
    }

    if (results.empty()) {
        return false;
    }

    dl::detect::result_t *best = NULL;
    int best_area = 0;
    
    for (auto &res : results) {
        int area = res.box[2] * res.box[3];
        if (area > best_area) {
            best_area = area;
            best = &res;
        }
    }

    if (!best) {
        return false;
    }

    out_box->x = best->box[0];
    out_box->y = best->box[1];
    out_box->width = best->box[2];
    out_box->height = best->box[3];
    out_box->score = best->score;

    ESP_LOGI(TAG, "Selected best: x=%d y=%d w=%d h=%d score=%.2f",
             out_box->x, out_box->y, out_box->width, out_box->height, out_box->score);
    
    ESP_LOGI(TAG, "Raw: x=%d y=%d w=%d h=%d", best->box[0], best->box[1], best->box[2], best->box[3]);
    ESP_LOGI(TAG, "Mapped: x1=%d y1=%d x2=%d y2=%d", 
         best->box[0] + 112, 
         best->box[1] - 20,
         best->box[0] + 112 + best->box[2],
         best->box[1] - 20 + best->box[3]);

    return true;
}