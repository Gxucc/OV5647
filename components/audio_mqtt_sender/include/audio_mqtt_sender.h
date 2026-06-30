#ifndef AUDIO_MQTT_SENDER_H
#define AUDIO_MQTT_SENDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 音频参数
#define SENDER_SAMPLE_RATE      16000
#define SENDER_DURATION_SEC     10
#define SENDER_TOTAL_SAMPLES    (SENDER_SAMPLE_RATE * SENDER_DURATION_SEC)

// MQTT 配置
#define MQTT_BROKER_URL         "mqtt://broker.emqx.io:1883"
#define MQTT_TOPIC_AUDIO        "homecare/audio/record"

// 分片大小（4KB payload）
#define SENDER_CHUNK_SIZE       4096

/**
 * @brief 初始化 WiFi 和 MQTT
 * @return ESP_OK on success
 */
esp_err_t audio_mqtt_sender_init(void);

/**
 * @brief 生成正弦波，ADPCM编码，通过MQTT发送
 * @param freq_hz 正弦波频率
 * @return ESP_OK on success
 */
esp_err_t audio_mqtt_sender_demo(float freq_hz);

/**
 * @brief 检查 WiFi 和 MQTT 是否已连接
 * @return true if connected
 */
bool audio_mqtt_sender_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif