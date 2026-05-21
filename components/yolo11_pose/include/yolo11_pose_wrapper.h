#ifndef YOLO11_POSE_WRAPPER_H
#define YOLO11_POSE_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YOLO11_NUM_KEYPOINTS     17
#define YOLO11_MAX_RESULTS       10

typedef struct {
    float score;
    int box[4];           // x1, y1, x2, y2
    float keypoint[34];   // 17 * 2 (x, y)
} yolo11_pose_result_t;

int yolo11_pose_init(void);
int yolo11_pose_run(const uint8_t *rgb888_buf, int width, int height);
int yolo11_pose_get_num_results(void);
int yolo11_pose_get_result(int idx, yolo11_pose_result_t *result);
void yolo11_pose_deinit(void);

#ifdef __cplusplus
}
#endif

#endif