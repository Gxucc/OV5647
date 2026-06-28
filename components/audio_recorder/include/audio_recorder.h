#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate;
    int bits_per_sample;
    int channel_count;
    int duration_ms;
    const char *save_path;
} audio_recorder_cfg_t;

esp_err_t audio_recorder_system_init(void *i2c_bus);
esp_err_t audio_recorder_start(const audio_recorder_cfg_t *cfg);
bool audio_recorder_is_running(void);
esp_err_t audio_recorder_stop(void);
void audio_recorder_system_deinit(void);
esp_err_t audio_recorder_read_samples(int16_t *buffer, size_t samples, int timeout_ms);

/**
 * @brief 保存 PCM 数据到 SD 卡（原始 int16，无 WAV 头）
 * @param path 文件路径，如 "/sdcard/env_10s.pcm"
 * @param buffer PCM 数据
 * @param samples 样本数
 * @return ESP_OK 成功
 */
esp_err_t audio_recorder_save_pcm(const char *path, const int16_t *buffer, size_t samples);

#ifdef __cplusplus
}
#endif

#endif