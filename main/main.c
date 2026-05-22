// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "esp_timer.h"
// #include "nvs_flash.h"
// #include "cam_net.h"
// #include "lcd_display.h"
// #include "net_service.h"

// static const char *TAG = "main";

// void app_main(void)
// {
//     ESP_LOGI(TAG, "App start");

//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);

//     // 初始化
//     ESP_ERROR_CHECK(cam_net_init());
//     ESP_ERROR_CHECK(lcd_display_init());
//     // ESP_ERROR_CHECK(net_service_wifi_init());
//     // ESP_ERROR_CHECK(net_service_http_start());

//     ESP_LOGI(TAG, "Ready!");

//     // 帧率统计
//     int64_t last_time = esp_timer_get_time();
//     int frame_count = 0;

//     while (1) {
//         uint8_t *rgb_buf = NULL;
//         uint32_t rgb_size = 0;

//         if (cam_net_get_rgb565(&rgb_buf, &rgb_size) == ESP_OK) 
//         {
//             // 1. LCD 显示（本地，全速）
//             lcd_display_camera(rgb_buf, 800, 800);

//             // // 2. JPEG 编码
//             // uint8_t *jpeg_buf = NULL;
//             // uint32_t jpeg_size = 0;
//             // if (cam_net_encode_jpeg(rgb_buf, rgb_size, &jpeg_buf, &jpeg_size) == ESP_OK) {
//             //     // 3. 放入队列（非阻塞，WiFi独立发送）
//             //     net_service_jpeg_update(jpeg_buf, jpeg_size);
//             // }

//             // 4. 归还 V4L2 缓冲
//             cam_net_release_rgb565();

//             // 帧率统计
//             frame_count++;
//             int64_t now = esp_timer_get_time();
//             if (now - last_time >= 1000000) {
//                 ESP_LOGI(TAG, "LCD FPS: %d", frame_count);
//                 frame_count = 0;
//                 last_time = now;
//             }
//         }

//        // vTaskDelay(pdMS_TO_TICKS(1));  // 最小延迟，让出CPU
//     }
// }
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cam_net.h"
#include "lcd_display.h"
#include "net_service.h"
#include "yolo11_pose_wrapper.h"

static const char *TAG = "main";

#define INFER_EVERY_N_FRAMES    3  // 每 3 帧推理 1 次
static int s_frame_counter = 0;

// 画关键点（坐标映射需根据实际模型输入调整）
static void draw_keypoints_on_lcd(uint8_t *rgb565_buf, int buf_w, int buf_h,
                                  yolo11_pose_result_t *result)
{
    // 模型内部用 letterbox，输出坐标是相对于模型输入的
    // 需要从 ImagePreprocessor 获取实际缩放比例
    // 暂时假设模型输入 640x640，letterbox 后有效区域居中
    
    float scale_x = 800.0f / 640.0f;   // 需根据实际调整
    float scale_y = 600.0f / 640.0f;
    int offset_x = (1024 - 800) / 2;
    
    for (int i = 0; i < YOLO11_NUM_KEYPOINTS; i++) {
        float kx = result->keypoint[i * 2 + 0];
        float ky = result->keypoint[i * 2 + 1];
        
        if (kx < 1.0f && ky < 1.0f) continue;
        
        int lcd_x = (int)(kx * scale_x) + offset_x;
        int lcd_y = (int)(ky * scale_y);
        
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                int px = lcd_x + dx;
                int py = lcd_y + dy;
                if (px >= 0 && px < buf_w && py >= 0 && py < buf_h) {
                    uint16_t *pixel = (uint16_t*)(rgb565_buf + (py * buf_w + px) * 2);
                    *pixel = 0xF800;
                }
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "App start");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(cam_net_init());
    ESP_ERROR_CHECK(lcd_display_init());
    // ESP_ERROR_CHECK(net_service_wifi_init());
    // ESP_ERROR_CHECK(net_service_http_start());
    
    if (yolo11_pose_init() != 0) {
        ESP_LOGE(TAG, "Model init failed");
        return;
    }

    ESP_LOGI(TAG, "Ready!");

    int64_t last_time = esp_timer_get_time();
    int frame_count = 0;
    int infer_count = 0;

    while (1) {
        uint8_t *rgb_buf = NULL;
        uint32_t rgb_size = 0;

        if (cam_net_get_rgb565(&rgb_buf, &rgb_size) == ESP_OK) {
            
            // 1. LCD 显示（每帧）
            lcd_display_camera(rgb_buf, 800, 800);
            
            // 2. 每 N 帧推理 1 次，直接传 RGB565
            if (++s_frame_counter % INFER_EVERY_N_FRAMES == 0) {
                int num = yolo11_pose_run_rgb565(rgb_buf, 800, 800);
                infer_count += num;
                
                for (int i = 0; i < num; i++) {
                    yolo11_pose_result_t result;
                    if (yolo11_pose_get_result(i, &result) == 0) {
                        ESP_LOGI(TAG, "Person %d: score=%.2f", i, result.score);
                        
                        uint8_t *lcd_buf = lcd_display_get_buffer();
                        if (lcd_buf) {
                            draw_keypoints_on_lcd(lcd_buf, 1024, 600, &result);
                        }
                    }
                }
                
                if (num > 0) {
                    lcd_display_flush();
                }
            }
            
            // // 3. JPEG + WiFi
            // uint8_t *jpeg_buf = NULL;
            // uint32_t jpeg_size = 0;
            // if (cam_net_encode_jpeg(rgb_buf, rgb_size, &jpeg_buf, &jpeg_size) == ESP_OK) {
            //     net_service_jpeg_update(jpeg_buf, jpeg_size);
            // }

            cam_net_release_rgb565();

            frame_count++;
            int64_t now = esp_timer_get_time();
            if (now - last_time >= 1000000) {
                ESP_LOGI(TAG, "LCD FPS: %d, Inferred: %d persons", frame_count, infer_count);
                frame_count = 0;
                infer_count = 0;
                last_time = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}