#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cam_net.h"
#include "lcd_display.h"   // 这里已经包含了 LCD_WIDTH / LCD_HEIGHT
#include "person_detect.h"
#include "draw_box.h"

static const char *TAG = "main";

#define CAM_WIDTH   1024
#define CAM_HEIGHT  600

void draw_reference_cross(uint8_t *buf, int buf_w, int buf_h)
{
    uint16_t *pixel_buf = (uint16_t *)buf;
    int cx = buf_w / 2;
    int cy = buf_h / 2;
    int size = 50;
    uint16_t color_green = 0x07E0;

    for (int x = cx - size; x <= cx + size; x++) {
        if (x >= 0 && x < buf_w) pixel_buf[cy * buf_w + x] = color_green;
    }
    for (int y = cy - size; y <= cy + size; y++) {
        if (y >= 0 && y < buf_h) pixel_buf[y * buf_w + cx] = color_green;
    }

    int margin = 30;
    uint16_t color_blue = 0x001F, color_red = 0xF800;
    uint16_t color_yellow = 0xFFE0, color_cyan = 0x07FF;

    for (int i = 0; i < 20; i++) {
        pixel_buf[margin * buf_w + (margin + i)] = color_blue;
        pixel_buf[(margin + i) * buf_w + margin] = color_blue;
        pixel_buf[margin * buf_w + (buf_w - margin - i)] = color_yellow;
        pixel_buf[(margin + i) * buf_w + (buf_w - margin)] = color_yellow;
        pixel_buf[(buf_h - margin) * buf_w + (margin + i)] = color_cyan;
        pixel_buf[(buf_h - margin - i) * buf_w + margin] = color_cyan;
        pixel_buf[(buf_h - margin) * buf_w + (buf_w - margin - i)] = color_red;
        pixel_buf[(buf_h - margin - i) * buf_w + (buf_w - margin)] = color_red;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(lcd_display_init());
    ESP_ERROR_CHECK(cam_net_init());
    ESP_ERROR_CHECK(person_detect_init());

    ESP_LOGI(TAG, "All modules initialized, entering main loop");

    while (1) {
        uint8_t *rgb_buf = NULL;
        uint32_t rgb_size = 0;

        if (cam_net_get_rgb565(&rgb_buf, &rgb_size) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get frame");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        lcd_display_camera(rgb_buf, CAM_WIDTH, CAM_HEIGHT);

        person_box_t box;
        bool detected = person_detect_run(rgb_buf, CAM_WIDTH, CAM_HEIGHT, &box);

        uint8_t *lcd_buf = lcd_display_get_buffer();
        draw_reference_cross(lcd_buf, LCD_WIDTH, LCD_HEIGHT);

        if (detected) {
            draw_box_on_lcd(lcd_buf, LCD_WIDTH, LCD_HEIGHT, &box, CAM_WIDTH, CAM_HEIGHT);

            // 修正：直接使用 lcd_buf，不要重复声明 pixel_buf
            uint16_t *pixels = (uint16_t *)lcd_buf;
            int center_x = box.x + box.width / 2;
            int center_y = box.y + box.height / 2;

            if (center_x >= 0 && center_x < LCD_WIDTH && center_y >= 0 && center_y < LCD_HEIGHT) {
                pixels[center_y * LCD_WIDTH + center_x] = 0xFFFF;
            }
        }

        lcd_display_flush();
        cam_net_release_rgb565();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}