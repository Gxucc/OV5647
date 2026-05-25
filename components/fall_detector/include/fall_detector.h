#ifndef FALL_DETECTOR_H
#define FALL_DETECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;
    float conf;
} fd_kpt_t;

typedef struct {
    fd_kpt_t kpts[6];
    float person_score;
    int valid;
} fd_result_t;

int fall_detector_init(void);
int fall_detector_run(const uint8_t *rgb565_buf, int src_w, int src_h, fd_result_t *out);
void fall_detector_deinit(void);

#ifdef __cplusplus
}
#endif

#endif