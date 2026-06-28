#ifndef BABYSOUND_DETECTOR_H
#define BABYSOUND_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BABYSOUND_TAG "babysound"

#define BABYSOUND_INPUT_FRAMES  61
#define BABYSOUND_INPUT_DIM     12
#define BABYSOUND_NUM_CLASSES   2

#define CLASS_NON_BABY  0
#define CLASS_BABY      1

#define BABYSOUND_CONFIDENCE_THRESHOLD 0.7f

typedef struct {
    float baby_prob;
    float non_baby_prob;
    int predicted_class;
    const char* label;
} babysound_result_t;

esp_err_t babysound_detector_init(void);
void babysound_detector_deinit(void);
esp_err_t babysound_detector_run(const int16_t *pcm_buffer, babysound_result_t *result);
bool babysound_detector_is_baby_cry(const int16_t *pcm_buffer, float threshold);

/**
 * @brief 调试：对 PCM 预处理并保存中间结果到 SD 卡
 * 
 * 保存以下文件：
 * - /sdcard/debug_mfcc_raw.txt    : 原始 MFCC [61, 12] float
 * - /sdcard/debug_mfcc_norm.txt   : 归一化后 [61, 12] float  
 * - /sdcard/debug_mfcc_quant.txt  : 量化后 [61, 12] int8
 * 
 * @param pcm_buffer 16000 个 int16_t 样本
 * @return ESP_OK 成功
 */
esp_err_t babysound_detector_debug_preprocess(const int16_t *pcm_buffer);

#ifdef __cplusplus
}
#endif

#endif