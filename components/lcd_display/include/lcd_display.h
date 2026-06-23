#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"

#define LCD_WIDTH   1024
#define LCD_HEIGHT  600

esp_err_t lcd_display_init(void);
void lcd_display_deinit(void);

void lcd_display_copy_camera(const uint8_t *rgb565_buf, uint32_t cam_width, uint32_t cam_height);
uint8_t *lcd_display_get_buffer(void);
uint8_t *lcd_display_get_buffer_idx(int idx);  // 新增：获取指定缓冲区
uint8_t lcd_display_get_buffer_count(void);    // 新增：获取缓冲区数量
void lcd_display_flush(void);
void lcd_display_clear(void);

esp_lcd_panel_handle_t lcd_display_get_panel(void);
esp_lcd_panel_io_handle_t lcd_display_get_io(void);

#endif