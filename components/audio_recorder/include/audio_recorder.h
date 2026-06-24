#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 录音配置参数
 */
typedef struct {
    int sample_rate;        // 采样率，默认 16000
    int bits_per_sample;    // 位深，默认 16
    int channel_count;      // 声道数，默认 1 (MONO)
    int duration_ms;        // 录音时长（毫秒），默认 10000
    const char *save_path;  // 保存路径，如 "/sdcard/rec.wav"
} audio_recorder_cfg_t;

/**
 * @brief 初始化录音系统（I2C + I2S + ES8311 + SD卡）
 * 
 * @param i2c_bus 已有的 I2C 总线句柄（i2c_master_bus_handle_t）
 * @return ESP_OK 成功
 */
esp_err_t audio_recorder_system_init(void *i2c_bus);

/**
 * @brief 开始录音并保存到文件
 * 
 * @param cfg 录音配置
 * @return ESP_OK 成功
 */
esp_err_t audio_recorder_start(const audio_recorder_cfg_t *cfg);

/**
 * @brief 获取录音状态
 * 
 * @return true 正在录音
 */
bool audio_recorder_is_running(void);

/**
 * @brief 停止当前录音
 * 
 * @return ESP_OK 成功
 */
esp_err_t audio_recorder_stop(void);

/**
 * @brief 反初始化录音系统
 */
void audio_recorder_system_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RECORDER_H