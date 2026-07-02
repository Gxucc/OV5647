#ifndef LVGL_UI_H
#define LVGL_UI_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    UI_STATE_HOME,
    UI_STATE_FALL_DEBUG,
    UI_STATE_RECORD,
    UI_STATE_BABYSOUND,
    UI_STATE_FACE_RECOGNITION,
} ui_state_t;

typedef enum {
    UI_SEND_STATUS_NONE,
    UI_SEND_STATUS_SENDING,
    UI_SEND_STATUS_SUCCESS,
    UI_SEND_STATUS_FAILED,
} ui_send_status_t;

// 报警类型（用于Toast显示）
typedef enum {
    UI_ALARM_NONE,
    UI_ALARM_FALL,
    UI_ALARM_BABY,
    UI_ALARM_STRANGER,
    UI_ALARM_NET_LOST,
} ui_alarm_type_t;

// 状态变化回调类型
typedef void (*lvgl_ui_state_change_cb_t)(ui_state_t prev_state, ui_state_t new_state);

esp_err_t lvgl_ui_init(void);
void lvgl_ui_deinit(void);
ui_state_t lvgl_ui_get_state(void);
void lvgl_ui_set_state(ui_state_t state);
void lvgl_ui_task_handler(void);

void lvgl_ui_register_state_change_cb(lvgl_ui_state_change_cb_t cb);

bool lvgl_ui_should_start_fall_debug(void);
bool lvgl_ui_should_start_record(void);
bool lvgl_ui_should_start_babysound(void);
bool lvgl_ui_should_start_face_recognition(void);
void lvgl_ui_clear_start_face_recognition(void);
bool lvgl_ui_should_return_home(void);
void lvgl_ui_set_return_home(void);

bool lvgl_ui_babysound_switch_is_on(void);
void lvgl_ui_set_babysound_switch(bool on);

bool lvgl_ui_should_toggle_record(void);
void lvgl_ui_clear_toggle_record(void);

void lvgl_ui_set_send_status(ui_send_status_t status);
void lvgl_ui_set_send_progress(int progress);

// ============ 新增：报警Toast接口 ============
void lvgl_ui_show_alarm_toast(ui_alarm_type_t type, const char *detail);
void lvgl_ui_show_net_status(bool connected);

#endif