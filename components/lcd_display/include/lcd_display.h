#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"

// 暴露 LCD 尺寸给外部使用
#define LCD_WIDTH   1024
#define LCD_HEIGHT  600

esp_err_t lcd_display_init(void);
void lcd_display_clear(void);
void lcd_display_camera(const uint8_t *rgb565_buf, uint32_t cam_width, uint32_t cam_height);
void lcd_display_flush(void);
uint8_t *lcd_display_get_buffer(void);

#endif