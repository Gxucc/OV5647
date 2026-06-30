#ifndef LVGL_UI_H
#define LVGL_UI_H

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    UI_STATE_HOME,
    UI_STATE_FALL_DEBUG,
    UI_STATE_RECORD,
    UI_STATE_BABYSOUND,
} ui_state_t;

typedef enum {
    UI_SEND_STATUS_NONE,
    UI_SEND_STATUS_SENDING,
    UI_SEND_STATUS_SUCCESS,
    UI_SEND_STATUS_FAILED,
} ui_send_status_t;

esp_err_t lvgl_ui_init(void);
void lvgl_ui_deinit(void);
ui_state_t lvgl_ui_get_state(void);
void lvgl_ui_set_state(ui_state_t state);
void lvgl_ui_task_handler(void);

bool lvgl_ui_should_start_fall_debug(void);
bool lvgl_ui_should_start_record(void);
bool lvgl_ui_should_start_babysound(void);
bool lvgl_ui_should_return_home(void);
void lvgl_ui_set_return_home(void);

bool lvgl_ui_babysound_switch_is_on(void);
void lvgl_ui_set_babysound_switch(bool on);

// 录音控制：用户点击了录音按钮，需要切换录音状态
bool lvgl_ui_should_toggle_record(void);
void lvgl_ui_clear_toggle_record(void);

// 发送状态设置（由外部任务更新）
void lvgl_ui_set_send_status(ui_send_status_t status);
void lvgl_ui_set_send_progress(int progress);

#endif