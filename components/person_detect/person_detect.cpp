#include "person_detect.h"
#include "pedestrian_detect.hpp"
#include "dl_image.hpp"
#include "esp_log.h"

static const char *TAG = "person_detect";
static PedestrianDetect *s_detector = NULL;

#define COLOR_RED    0xF800
#define COLOR_GREEN  0x07E0
#define COLOR_BLUE   0x001F
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN   0x07FF
#define COLOR_WHITE  0xFFFF

static inline void set_pixel(uint16_t *buf, int x, int y, int width, int height, uint16_t color)
{
    if (x >= 0 && x < width && y >= 0 && y < height) {
        buf[y * width + x] = color;
    }
}

static inline void draw_hline(uint16_t *buf, int x1, int x2, int y, int width, int height, uint16_t color)
{
    if (y < 0 || y >= height) return;
    if (x1 < 0) x1 = 0;
    if (x2 >= width) x2 = width - 1;
    if (x1 > x2) return;
    
    uint16_t *line = &buf[y * width];
    for (int x = x1; x <= x2; x++) {
        line[x] = color;
    }
}

static inline void draw_vline(uint16_t *buf, int x, int y1, int y2, int width, int height, uint16_t color)
{
    if (x < 0 || x >= width) return;
    if (y1 < 0) y1 = 0;
    if (y2 >= height) y2 = height - 1;
    if (y1 > y2) return;
    
    for (int y = y1; y <= y2; y++) {
        buf[y * width + x] = color;
    }
}

static void draw_reference_cross(uint16_t *pixel_buf, int buf_w, int buf_h)
{
    int cx = buf_w / 2;
    int cy = buf_h / 2;
    int size = 50;

    draw_hline(pixel_buf, cx - size, cx + size, cy, buf_w, buf_h, COLOR_GREEN);
    draw_vline(pixel_buf, cx, cy - size, cy + size, buf_w, buf_h, COLOR_GREEN);

    int margin = 30;
    int len = 20;
    
    draw_hline(pixel_buf, margin, margin + len, margin, buf_w, buf_h, COLOR_BLUE);
    draw_vline(pixel_buf, margin, margin, margin + len, buf_w, buf_h, COLOR_BLUE);
    
    draw_hline(pixel_buf, buf_w - margin - len, buf_w - margin, margin, buf_w, buf_h, COLOR_YELLOW);
    draw_vline(pixel_buf, buf_w - margin, margin, margin + len, buf_w, buf_h, COLOR_YELLOW);
    
    draw_hline(pixel_buf, margin, margin + len, buf_h - margin, buf_w, buf_h, COLOR_CYAN);
    draw_vline(pixel_buf, margin, buf_h - margin - len, buf_h - margin, buf_w, buf_h, COLOR_CYAN);
    
    draw_hline(pixel_buf, buf_w - margin - len, buf_w - margin, buf_h - margin, buf_w, buf_h, COLOR_RED);
    draw_vline(pixel_buf, buf_w - margin, buf_h - margin - len, buf_h - margin, buf_w, buf_h, COLOR_RED);
}

static void draw_box_on_buffer(uint16_t *pixel_buf, int buf_w, int buf_h,
                               const person_box_t *box)
{
    if (!pixel_buf || !box || box->width <= 0 || box->height <= 0) {
        return;
    }

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
        draw_hline(pixel_buf, x1, x2, y1 + t, buf_w, buf_h, COLOR_RED);
        draw_hline(pixel_buf, x1, x2, y2 - t, buf_w, buf_h, COLOR_RED);
        draw_vline(pixel_buf, x1 + t, y1, y2, buf_w, buf_h, COLOR_RED);
        draw_vline(pixel_buf, x2 - t, y1, y2, buf_w, buf_h, COLOR_RED);
    }
}

static void draw_center_point(uint16_t *pixel_buf, int buf_w, int buf_h,
                              const person_box_t *box)
{
    int center_x = box->x + box->width / 2;
    int center_y = box->y + box->height / 2;

    if (center_x >= 0 && center_x < buf_w && center_y >= 0 && center_y < buf_h) {
        pixel_buf[center_y * buf_w + center_x] = COLOR_WHITE;
    }
}

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

extern "C" void person_detect_deinit(void)
{
    if (s_detector) {
        delete s_detector;
        s_detector = NULL;
        ESP_LOGI(TAG, "Person detector deinitialized");
    }
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
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
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

    ESP_LOGI(TAG, "Selected: x=%d y=%d w=%d h=%d score=%.2f",
             out_box->x, out_box->y, out_box->width, out_box->height, out_box->score);

    return true;
}

extern "C" bool person_detect_process(const uint8_t *rgb565_buf, int cam_w, int cam_h,
                                      uint8_t *lcd_buf, int lcd_w, int lcd_h)
{
    if (!lcd_buf) {
        return false;
    }

    uint16_t *pixel_buf = (uint16_t *)lcd_buf;

    draw_reference_cross(pixel_buf, lcd_w, lcd_h);

    person_box_t box;
    bool detected = person_detect_run(rgb565_buf, cam_w, cam_h, &box);

    if (detected) {
        draw_box_on_buffer(pixel_buf, lcd_w, lcd_h, &box);
        draw_center_point(pixel_buf, lcd_w, lcd_h, &box);
    }

    return detected;
}