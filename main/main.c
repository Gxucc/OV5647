// #include <stdio.h>
// #include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "esp_timer.h"
// #include "nvs_flash.h"
// #include "cam_net.h"
// #include "lcd_display.h"
// #include "fall_detector.h"

// static const char *TAG = "main";

// static const uint16_t KPT_COLORS[6] = {
//     0xF800, 0xF800, 0x07E0, 0x07E0, 0x001F, 0x001F
// };

// static void draw_point(uint16_t *pixels, int x, int y, int w, int h, uint16_t color, int size)
// {
//     for (int dy = -size; dy <= size; dy++) {
//         for (int dx = -size; dx <= size; dx++) {
//             int px = x + dx;
//             int py = y + dy;
//             if (px >= 0 && px < w && py >= 0 && py < h) {
//                 pixels[py * w + px] = color;
//             }
//         }
//     }
// }

// static void draw_line(uint16_t *pixels, int x1, int y1, int x2, int y2, int w, int h, uint16_t color)
// {
//     int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
//     int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
//     int err = dx + dy;
    
//     while (1) {
//         if (x1 >= 0 && x1 < w && y1 >= 0 && y1 < h) {
//             pixels[y1 * w + x1] = color;
//         }
//         if (x1 == x2 && y1 == y2) break;
//         int e2 = 2 * err;
//         if (e2 >= dy) { err += dy; x1 += sx; }
//         if (e2 <= dx) { err += dx; y1 += sy; }
//     }
// }

// static void draw_results_on_camera(uint8_t *rgb565_buf, int cam_w, int cam_h, fd_result_t *result)
// {
//     if (!result->valid) return;
    
//     uint16_t *pixels = (uint16_t*)rgb565_buf;
    
//     for (int k = 0; k < 6; k++) {
//         int x = (int)(result->kpts[k].x * cam_w);
//         int y = (int)(result->kpts[k].y * cam_h);
//         draw_point(pixels, x, y, cam_w, cam_h, KPT_COLORS[k], 3);
//     }
    
//     int lx0 = (int)(result->kpts[0].x * cam_w), ly0 = (int)(result->kpts[0].y * cam_h);
//     int lx1 = (int)(result->kpts[2].x * cam_w), ly1 = (int)(result->kpts[2].y * cam_h);
//     int lx2 = (int)(result->kpts[4].x * cam_w), ly2 = (int)(result->kpts[4].y * cam_h);
//     draw_line(pixels, lx0, ly0, lx1, ly1, cam_w, cam_h, 0xFFFF);
//     draw_line(pixels, lx1, ly1, lx2, ly2, cam_w, cam_h, 0xFFFF);
    
//     int rx0 = (int)(result->kpts[1].x * cam_w), ry0 = (int)(result->kpts[1].y * cam_h);
//     int rx1 = (int)(result->kpts[3].x * cam_w), ry1 = (int)(result->kpts[3].y * cam_h);
//     int rx2 = (int)(result->kpts[5].x * cam_w), ry2 = (int)(result->kpts[5].y * cam_h);
//     draw_line(pixels, rx0, ry0, rx1, ry1, cam_w, cam_h, 0xFFFF);
//     draw_line(pixels, rx1, ry1, rx2, ry2, cam_w, cam_h, 0xFFFF);
    
//     draw_line(pixels, lx0, ly0, rx0, ry0, cam_w, cam_h, 0xFFE0);
// }

// void app_main(void)
// {
//     ESP_LOGI(TAG, "App start");

//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);

//     ESP_ERROR_CHECK(cam_net_init());
//     ESP_ERROR_CHECK(lcd_display_init());

//     if (fall_detector_init() != 0) {
//         ESP_LOGE(TAG, "Model init failed");
//         return;
//     }

//     ESP_LOGI(TAG, "Ready!");

//     int64_t last_time = esp_timer_get_time();
//     int frame_count = 0;

//     while (1) {
//         uint8_t *rgb_buf = NULL;
//         uint32_t rgb_size = 0;

//         if (cam_net_get_rgb565(&rgb_buf, &rgb_size) == ESP_OK) {
//             fd_result_t result;
//             //fall_detector_run(rgb_buf, 800, 640, &result);
            
//             if (result.valid) {
//               //  draw_results_on_camera(rgb_buf, 800, 640, &result);
//             }
            
//             lcd_display_camera(rgb_buf, 800, 640);

//             cam_net_release_rgb565();

//             frame_count++;
//             int64_t now = esp_timer_get_time();
//             if (now - last_time >= 1000000) {
//                 ESP_LOGI(TAG, "FPS: %d", frame_count);
//                 frame_count = 0;
//                 last_time = now;
//             }
//         }

//         vTaskDelay(pdMS_TO_TICKS(1));
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
#include "esp_heap_caps.h"
#include "lcd_display.h"
#include "fall_detector.h"
#include "test_image.h"

static const char *TAG = "main";

static const uint16_t KPT_COLORS[6] = {
    0xF800, 0xF800, 0x07E0, 0x07E0, 0x001F, 0x001F
};

static void draw_point(uint16_t *pixels, int x, int y, int w, int h, uint16_t color, int size)
{
    for (int dy = -size; dy <= size; dy++) {
        for (int dx = -size; dx <= size; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < w && py >= 0 && py < h) {
                pixels[py * w + px] = color;
            }
        }
    }
}

static void draw_line(uint16_t *pixels, int x1, int y1, int x2, int y2, int w, int h, uint16_t color)
{
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;
    
    while (1) {
        if (x1 >= 0 && x1 < w && y1 >= 0 && y1 < h) {
            pixels[y1 * w + x1] = color;
        }
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

static void draw_results_on_camera(uint8_t *rgb565_buf, int cam_w, int cam_h, fd_result_t *result)
{
    if (!result->valid) return;
    
    uint16_t *pixels = (uint16_t*)rgb565_buf;
    
    for (int k = 0; k < 6; k++) {
        int x = (int)(result->kpts[k].x * cam_w);
        int y = (int)(result->kpts[k].y * cam_h);
        draw_point(pixels, x, y, cam_w, cam_h, KPT_COLORS[k], 3);
    }
    
    int lx0 = (int)(result->kpts[0].x * cam_w), ly0 = (int)(result->kpts[0].y * cam_h);
    int lx1 = (int)(result->kpts[2].x * cam_w), ly1 = (int)(result->kpts[2].y * cam_h);
    int lx2 = (int)(result->kpts[4].x * cam_w), ly2 = (int)(result->kpts[4].y * cam_h);
    draw_line(pixels, lx0, ly0, lx1, ly1, cam_w, cam_h, 0xFFFF);
    draw_line(pixels, lx1, ly1, lx2, ly2, cam_w, cam_h, 0xFFFF);
    
    int rx0 = (int)(result->kpts[1].x * cam_w), ry0 = (int)(result->kpts[1].y * cam_h);
    int rx1 = (int)(result->kpts[3].x * cam_w), ry1 = (int)(result->kpts[3].y * cam_h);
    int rx2 = (int)(result->kpts[5].x * cam_w), ry2 = (int)(result->kpts[5].y * cam_h);
    draw_line(pixels, rx0, ry0, rx1, ry1, cam_w, cam_h, 0xFFFF);
    draw_line(pixels, rx1, ry1, rx2, ry2, cam_w, cam_h, 0xFFFF);
    
    draw_line(pixels, lx0, ly0, rx0, ry0, cam_w, cam_h, 0xFFE0);
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

    ESP_ERROR_CHECK(lcd_display_init());

    if (fall_detector_init() != 0) {
        ESP_LOGE(TAG, "Model init failed");
        return;
    }

    ESP_LOGI(TAG, "Ready! Processing static image...");

    // 使用固定变量名
    const uint8_t *rgb_buf = test_image;
    uint32_t rgb_size = test_image_len;

    uint8_t *draw_buf = heap_caps_malloc(TEST_IMAGE_WIDTH * TEST_IMAGE_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!draw_buf) {
        ESP_LOGE(TAG, "Failed to alloc draw_buf in PSRAM");
        return;
    }

    while (1) {
        memcpy(draw_buf, rgb_buf, rgb_size);

        fd_result_t result;
        fall_detector_run(draw_buf, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT, &result);
        
        if (result.valid) {
            ESP_LOGI(TAG, "Detected! score=%.3f", result.person_score);
            for (int k = 0; k < 6; k++) {
                ESP_LOGI(TAG, "  Kpt%d: (%.3f, %.3f) conf=%.3f", 
                         k, result.kpts[k].x, result.kpts[k].y, result.kpts[k].conf);
            }
            draw_results_on_camera(draw_buf, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT, &result);
        } else {
            ESP_LOGW(TAG, "No detection");
        }
        
        lcd_display_camera(draw_buf, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}