#ifndef PERSON_DETECT_H
#define PERSON_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
    int width;
    int height;
    float score;
} person_box_t;

esp_err_t person_detect_init(void);
void person_detect_deinit(void);

// 纯检测API
bool person_detect_run(const uint8_t *rgb565_buf, int img_width, int img_height,
                       person_box_t *out_box);

// 检测 + 画框（在 lcd_buf 上）
bool person_detect_process(const uint8_t *rgb565_buf, int cam_w, int cam_h,
                           uint8_t *lcd_buf, int lcd_w, int lcd_h);

#ifdef __cplusplus
}
#endif

#endif