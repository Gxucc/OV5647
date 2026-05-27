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
bool person_detect_run(const uint8_t *rgb565_buf, int img_width, int img_height,
                       person_box_t *out_box);

#ifdef __cplusplus
}
#endif

#endif