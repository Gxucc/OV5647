#include "mqtt_service.h"
#include "audio_mqtt_sender.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_service";

#define ALARM_TOPIC         "homecare/alarm"
#define DEFAULT_COOLDOWN_S  30
#define STRANGER_THRESHOLD  3   // 连续3帧陌生人触发报警

// ============ 冷却状态 ============
typedef struct {
    alarm_type_t type;
    int64_t last_alarm_us;
} alarm_cooldown_t;

static alarm_cooldown_t s_cooldowns[3] = {0};
static uint32_t s_cooldown_sec = DEFAULT_COOLDOWN_S;
static volatile alarm_status_t s_alarm_status = ALARM_STATUS_IDLE;

// ============ 陌生人计数器 ============
static int s_stranger_count = 0;

// ============ 工具函数 ============
static const char* alarm_type_to_str(alarm_type_t type)
{
    switch (type) {
        case ALARM_TYPE_FALL:          return "fall_detected";
        case ALARM_TYPE_BABY_SOUND:    return "baby_sound_detected";
        case ALARM_TYPE_STRANGER_FACE: return "stranger_face_detected";
        default: return "unknown";
    }
}

static int alarm_type_to_idx(alarm_type_t type)
{
    switch (type) {
        case ALARM_TYPE_FALL:          return 0;
        case ALARM_TYPE_BABY_SOUND:    return 1;
        case ALARM_TYPE_STRANGER_FACE: return 2;
        default: return -1;
    }
}

static bool check_cooldown(alarm_type_t type)
{
    int idx = alarm_type_to_idx(type);
    if (idx < 0) return false;

    int64_t now = esp_timer_get_time();
    int64_t cooldown_us = (int64_t)s_cooldown_sec * 1000000LL;

    if (now - s_cooldowns[idx].last_alarm_us < cooldown_us) {
        return true;  // 仍在冷却中
    }

    s_cooldowns[idx].last_alarm_us = now;
    return false;
}

// ============ 核心发送函数 ============
static esp_err_t send_alarm_json(alarm_type_t type, float confidence, const char *detail)
{
    if (!audio_mqtt_sender_is_ready()) {
        ESP_LOGW(TAG, "Alarm %s: WiFi/MQTT not ready, dropped", alarm_type_to_str(type));
        s_alarm_status = ALARM_STATUS_FAILED_UNCONNECTED;
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON");
        s_alarm_status = ALARM_STATUS_FAILED_UNCONNECTED;
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "type", alarm_type_to_str(type));
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "confidence", confidence);
    if (detail && strlen(detail) > 0) {
        cJSON_AddStringToObject(root, "detail", detail);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "Failed to print JSON");
        s_alarm_status = ALARM_STATUS_FAILED_UNCONNECTED;
        return ESP_FAIL;
    }

    s_alarm_status = ALARM_STATUS_SENDING;
    ESP_LOGI(TAG, "Sending alarm: %s", json_str);

    esp_mqtt_client_handle_t client = audio_mqtt_sender_get_client();
    if (!client) {
        ESP_LOGE(TAG, "MQTT client not available");
        free(json_str);
        s_alarm_status = ALARM_STATUS_FAILED_UNCONNECTED;
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(
        client,
        ALARM_TOPIC,
        json_str,
        strlen(json_str),
        0, 0
    );

    free(json_str);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Alarm publish failed");
        s_alarm_status = ALARM_STATUS_FAILED_UNCONNECTED;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Alarm sent, msg_id=%d", msg_id);
    s_alarm_status = ALARM_STATUS_SUCCESS;
    return ESP_OK;
}

// ============ 公共 API ============

esp_err_t mqtt_service_init(void)
{
    ESP_LOGI(TAG, "MQTT service init (alarm reporter)");
    memset(s_cooldowns, 0, sizeof(s_cooldowns));
    s_stranger_count = 0;
    s_alarm_status = ALARM_STATUS_IDLE;
    return ESP_OK;
}

esp_err_t mqtt_service_alarm_fall(float confidence, float box_area_ratio)
{
    // 面积过大不报警（人太近，可能是正常站立）
    if (box_area_ratio >= 0.6f) {
        ESP_LOGD(TAG, "Fall: box area %.2f >= 60%%, skip alarm", box_area_ratio);
        return ESP_OK;  // 不是错误，只是条件不满足
    }

    // 冷却检查
    if (check_cooldown(ALARM_TYPE_FALL)) {
        ESP_LOGI(TAG, "Fall alarm in cooldown, skipped");
        s_alarm_status = ALARM_STATUS_FAILED_COOLDOWN;
        return ESP_OK;
    }

    char detail[64];
    snprintf(detail, sizeof(detail), "box_area_ratio: %.2f", box_area_ratio);

    return send_alarm_json(ALARM_TYPE_FALL, confidence, detail);
}

esp_err_t mqtt_service_alarm_baby(float confidence)
{
    if (check_cooldown(ALARM_TYPE_BABY_SOUND)) {
        ESP_LOGI(TAG, "Baby alarm in cooldown, skipped");
        s_alarm_status = ALARM_STATUS_FAILED_COOLDOWN;
        return ESP_OK;
    }

    return send_alarm_json(ALARM_TYPE_BABY_SOUND, confidence, "baby sound detected");
}

esp_err_t mqtt_service_alarm_stranger(float confidence)
{
    if (check_cooldown(ALARM_TYPE_STRANGER_FACE)) {
        ESP_LOGI(TAG, "Stranger alarm in cooldown, skipped");
        s_alarm_status = ALARM_STATUS_FAILED_COOLDOWN;
        return ESP_OK;
    }

    char detail[64];
    snprintf(detail, sizeof(detail), "similarity: %.3f", confidence);

    return send_alarm_json(ALARM_TYPE_STRANGER_FACE, 1.0f - confidence, detail);
}

alarm_status_t mqtt_service_get_alarm_status(void)
{
    return s_alarm_status;
}

void mqtt_service_set_cooldown_sec(uint32_t sec)
{
    s_cooldown_sec = sec;
    ESP_LOGI(TAG, "Cooldown set to %d seconds", sec);
}

// ============ 陌生人计数封装 ============
void mqtt_service_stranger_frame(bool is_stranger, float similarity)
{
    if (is_stranger) {
        s_stranger_count++;
        ESP_LOGD(TAG, "Stranger frame count: %d/%d", s_stranger_count, STRANGER_THRESHOLD);
        if (s_stranger_count >= STRANGER_THRESHOLD) {
            mqtt_service_alarm_stranger(similarity);
            s_stranger_count = 0;  // 触发后重置
        }
    } else {
        if (s_stranger_count > 0) {
            ESP_LOGD(TAG, "Stranger count reset (recognized)");
        }
        s_stranger_count = 0;
    }
}

void mqtt_service_reset_stranger_count(void)
{
    if (s_stranger_count > 0) {
        ESP_LOGD(TAG, "Stranger count reset (exit face recognition)");
    }
    s_stranger_count = 0;
}
