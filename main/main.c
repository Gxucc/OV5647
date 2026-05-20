#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cam_net.h"
#include "lcd_display.h"
#include "net_service.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "App start");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化
    ESP_ERROR_CHECK(cam_net_init());
    ESP_ERROR_CHECK(lcd_display_init());
    // ESP_ERROR_CHECK(net_service_wifi_init());
    // ESP_ERROR_CHECK(net_service_http_start());

    ESP_LOGI(TAG, "Ready!");

    // 帧率统计
    int64_t last_time = esp_timer_get_time();
    int frame_count = 0;

    while (1) {
        uint8_t *rgb_buf = NULL;
        uint32_t rgb_size = 0;

        if (cam_net_get_rgb565(&rgb_buf, &rgb_size) == ESP_OK) 
        {
            // 1. LCD 显示（本地，全速）
            lcd_display_camera(rgb_buf, 800, 800);

            // // 2. JPEG 编码
            // uint8_t *jpeg_buf = NULL;
            // uint32_t jpeg_size = 0;
            // if (cam_net_encode_jpeg(rgb_buf, rgb_size, &jpeg_buf, &jpeg_size) == ESP_OK) {
            //     // 3. 放入队列（非阻塞，WiFi独立发送）
            //     net_service_jpeg_update(jpeg_buf, jpeg_size);
            // }

            // 4. 归还 V4L2 缓冲
            cam_net_release_rgb565();

            // 帧率统计
            frame_count++;
            int64_t now = esp_timer_get_time();
            if (now - last_time >= 1000000) {
                ESP_LOGI(TAG, "LCD FPS: %d", frame_count);
                frame_count = 0;
                last_time = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // 最小延迟，让出CPU
    }
}