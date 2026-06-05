#include "draw_box.h"

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

    (void)cam_w; (void)cam_h;  // 现在分辨率一致，不需要映射

    uint16_t *pixel_buf = (uint16_t *)buf;

    int x1 = box->x;
    int y1 = box->y;
    int x2 = box->x + box->width;
    int y2 = box->y + box->height;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= buf_w) x2 = buf_w - 1;
    if (y2 >= buf_h) y2 = buf_h - 1;
    
    if (x1 >= x2 || y1 >= y2) return;

    int line_width = 3;
    for (int t = 0; t < line_width; t++) {
        for (int x = x1; x <= x2; x++) {
            set_pixel(pixel_buf, x, y1 + t, buf_w, buf_h, COLOR_RED);
            set_pixel(pixel_buf, x, y2 - t, buf_w, buf_h, COLOR_RED);
        }
        for (int y = y1; y <= y2; y++) {
            set_pixel(pixel_buf, x1 + t, y, buf_w, buf_h, COLOR_RED);
            set_pixel(pixel_buf, x2 - t, y, buf_w, buf_h, COLOR_RED);
        }
    }
}