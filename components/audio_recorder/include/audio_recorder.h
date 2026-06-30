#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;
    int bits_per_sample;
    int channel_count;
    int duration_ms;
} audio_recorder_cfg_t;

// 录音完成回调：buffer 为录音数据（PCM int16），samples 为采样点数，user_ctx 为用户上下文
typedef void (*audio_recorder_done_cb_t)(const int16_t *buffer, size_t samples, void *user_ctx);

// 系统级初始化/反初始化（包含 I2S、Codec、统一采集任务）
esp_err_t audio_recorder_system_init(void *i2c_bus);
void audio_recorder_system_deinit(void);

// 录音功能（录音到内存缓冲区，完成后通过回调通知）
esp_err_t audio_recorder_start(const audio_recorder_cfg_t *cfg, audio_recorder_done_cb_t done_cb, void *user_ctx);
esp_err_t audio_recorder_stop(void);
bool audio_recorder_is_running(void);

// 实时采样读取（供婴儿声音检测等模块使用，从统一采集缓冲区读取）
esp_err_t audio_recorder_read_samples(int16_t *buffer, size_t samples, int timeout_ms);

// 统一音频采集控制
esp_err_t audio_capture_start(void);   // 启动后台采集任务
void audio_capture_stop(void);         // 停止后台采集任务
bool audio_capture_is_running(void);

#ifdef __cplusplus
}
#endif

#endif