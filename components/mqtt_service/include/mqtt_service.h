#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============ 报警类型 ============
typedef enum {
    ALARM_TYPE_FALL,
    ALARM_TYPE_BABY_SOUND,
    ALARM_TYPE_STRANGER_FACE,
} alarm_type_t;

// ============ 报警状态 ============
typedef enum {
    ALARM_STATUS_IDLE,
    ALARM_STATUS_SENDING,
    ALARM_STATUS_SUCCESS,
    ALARM_STATUS_FAILED_UNCONNECTED,
    ALARM_STATUS_FAILED_COOLDOWN,   // 冷却中，不重复发送
} alarm_status_t;

// ============ 初始化 ============
esp_err_t mqtt_service_init(void);

// ============ 报警上报接口 ============
// 跌倒检测报警：confidence=跌倒置信度, box_area_ratio=框面积占比(0.0~1.0)
// 内部自动判断：box_area_ratio >= 0.6 时不触发报警
esp_err_t mqtt_service_alarm_fall(float confidence, float box_area_ratio);

// 婴儿声音报警：confidence=移动平均后的置信度
esp_err_t mqtt_service_alarm_baby(float confidence);

// 陌生人脸报警：由调用方维护连续计数，达到3次后直接调用
// confidence=相似度(陌生人时通常较低)
esp_err_t mqtt_service_alarm_stranger(float confidence);

// ============ 状态查询 ============
alarm_status_t mqtt_service_get_alarm_status(void);

// ============ 冷却时间配置 ============
void mqtt_service_set_cooldown_sec(uint32_t sec);  // 默认30秒

// ============ 陌生人计数封装（简化 main.c 调用） ============
// 每帧调用，传入是否为陌生人及相似度，内部维护连续计数，达到3次自动报警
void mqtt_service_stranger_frame(bool is_stranger, float similarity);

// 退出人脸识别界面时调用，重置计数器
void mqtt_service_reset_stranger_count(void);

// ============ 预留：手机端控制接口（后续扩展） ============
// typedef enum { CMD_LED_ON, CMD_LED_OFF, ... } device_cmd_t;
// esp_err_t mqtt_service_register_cmd_cb(void (*cb)(device_cmd_t cmd, void *arg));
// esp_err_t mqtt_service_send_status(const char *status_json);

#ifdef __cplusplus
}
#endif

#endif
