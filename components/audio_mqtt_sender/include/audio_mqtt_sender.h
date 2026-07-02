#ifndef AUDIO_MQTT_SENDER_H
#define AUDIO_MQTT_SENDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "mqtt_client.h"

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

// 连接重试配置
#define MAX_CONNECT_RETRY       5

// 发送状态
typedef enum {
    SEND_STATUS_IDLE,
    SEND_STATUS_SENDING,
    SEND_STATUS_SUCCESS,
    SEND_STATUS_FAILED,
} send_status_t;

esp_err_t audio_mqtt_sender_init(void);
bool audio_mqtt_sender_is_ready(void);
esp_err_t audio_mqtt_sender_send_wav(const char *wav_path);
esp_err_t audio_mqtt_sender_send_pcm(const int16_t *pcm_data, size_t samples);
send_status_t audio_mqtt_sender_get_status(void);
int audio_mqtt_sender_get_progress(void);
void audio_mqtt_sender_reset_connection(void);
esp_err_t audio_mqtt_sender_demo(float freq_hz);
esp_mqtt_client_handle_t audio_mqtt_sender_get_client(void);

#ifdef __cplusplus
}
#endif

#endif