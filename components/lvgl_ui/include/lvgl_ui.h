#ifndef LVGL_UI_H
#define LVGL_UI_H

#include <stdbool.h>
#include "esp_err.h"

// 界面状态
typedef enum {
    UI_STATE_HOME,
    UI_STATE_FALL_DEBUG,
    UI_STATE_RECORD,
    UI_STATE_BABYSOUND,
} ui_state_t;

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

// 删除这些：
// bool lvgl_ui_should_pause_detect(void);
// bool lvgl_ui_should_resume_detect(void);
// bool lvgl_ui_should_pause_babysound(void);
// bool lvgl_ui_should_resume_babysound(void);

// 保留开关状态
bool lvgl_ui_babysound_switch_is_on(void);
void lvgl_ui_set_babysound_switch(bool on);

#endif