#include "draw_box.h"
#include <string.h>

// RGB565 红色
#define COLOR_RED  0xF800

static inline void set_pixel(uint16_t *buf, int x, int y, int width, int height, uint16_t color)
{
    if (x >= 0 && x < width && y >= 0 && y < height) {
        buf[y * width + x] = color;
    }
}

void draw_box_on_lcd(uint8_t *buf, int buf_w, int buf_h,
                     const person_box_t *box, int cam_w, int cam_h)
{
    if (!buf || !box || box->width <= 0 || box->height <= 0) {
        return;
    }

    uint16_t *pixel_buf = (uint16_t *)buf;

    int src_y_start = (cam_h - buf_h) / 2;   // 垂直裁剪偏移: (640-600)/2 = 20
    int dst_x_offset = (buf_w - cam_w) / 2;  // 水平居中偏移: (1024-800)/2 = 112

    // 将摄像头原始坐标映射到 LCD 坐标（以左上角为基准）
    int x1 = box->x + dst_x_offset;
    int y1 = box->y - src_y_start;
    int x2 = x1 + box->width;
    int y2 = y1 + box->height;

    // 裁剪到 LCD 边界内
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= buf_w) x2 = buf_w - 1;
    if (y2 >= buf_h) y2 = buf_h - 1;

    // 画框（3像素宽）
    int line_width = 3;
    for (int t = 0; t < line_width; t++) {
        // 上下边
        for (int x = x1; x <= x2; x++) {
            set_pixel(pixel_buf, x, y1 + t, buf_w, buf_h, COLOR_RED);
            set_pixel(pixel_buf, x, y2 - t, buf_w, buf_h, COLOR_RED);
        }
        // 左右边
        for (int y = y1; y <= y2; y++) {
            set_pixel(pixel_buf, x1 + t, y, buf_w, buf_h, COLOR_RED);
            set_pixel(pixel_buf, x2 - t, y, buf_w, buf_h, COLOR_RED);
        }
    }
}