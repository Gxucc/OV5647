#include "yolo11_pose_wrapper.h"
#include "coco_pose.hpp"
#include "dl_image.hpp"
#include "dl_detect_base.hpp"
#include "esp_log.h"

static const char *TAG = "yolo11_pose_wrapper";
static COCOPose *s_pose = nullptr;
static std::list<dl::detect::result_t> s_results;

extern "C" {

int yolo11_pose_init(void)
{
    if (s_pose) return 0;
    
    s_pose = new COCOPose(COCOPose::model_type_t::YOLO11N_POSE_S8_V1);
    if (!s_pose) {
        ESP_LOGE(TAG, "Failed to allocate COCOPose");
        return -1;
    }
    
    ESP_LOGI(TAG, "Model initialized");
    return 0;
}

int yolo11_pose_run(const uint8_t *rgb888_buf, int width, int height)
{
    if (!s_pose || !rgb888_buf) return -1;
    
    dl::image::img_t img;
    img.data = (void*)rgb888_buf;
    img.width = width;
    img.height = height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;
    
    s_results = s_pose->run(img);
    return s_results.size();
}

// 新增：RGB565 接口，ImagePreprocessor 内部硬件加速转换
int yolo11_pose_run_rgb565(const uint8_t *rgb565_buf, int width, int height)
{
    if (!s_pose || !rgb565_buf) return -1;
    
    dl::image::img_t img;
    img.data = (void*)rgb565_buf;
    img.width = width;
    img.height = height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;  // ← OV5647 是 little-endian
    
    s_results = s_pose->run(img);
    ESP_LOGI(TAG, "Detected %d persons", (int)s_results.size());
    return s_results.size();
}

int yolo11_pose_get_num_results(void)
{
    return s_results.size();
}

int yolo11_pose_get_result(int idx, yolo11_pose_result_t *result)
{
    if (idx < 0 || idx >= (int)s_results.size() || !result) return -1;
    
    auto it = s_results.begin();
    std::advance(it, idx);
    const auto &res = *it;
    
    result->score = res.score;
    result->box[0] = res.box[0];
    result->box[1] = res.box[1];
    result->box[2] = res.box[2];
    result->box[3] = res.box[3];
    
    for (int i = 0; i < YOLO11_NUM_KEYPOINTS; i++) {
        result->keypoint[i * 2 + 0] = res.keypoint[i * 2 + 0];
        result->keypoint[i * 2 + 1] = res.keypoint[i * 2 + 1];
    }
    return 0;
}

void yolo11_pose_deinit(void)
{
    if (s_pose) {
        delete s_pose;
        s_pose = nullptr;
        s_results.clear();
    }
}

} // extern "C"