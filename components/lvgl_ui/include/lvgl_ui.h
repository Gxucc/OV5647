#ifndef LVGL_UI_H
#define LVGL_UI_H

#include <stdbool.h>
#include "esp_err.h"

// 界面状态
typedef enum {
    UI_STATE_HOME,       // 主界面
    UI_STATE_FALL_DEBUG, // 跌倒检测调试界面
    UI_STATE_RECORD,     // 录音界面
} ui_state_t;

// 初始化 LVGL 显示系统
esp_err_t lvgl_ui_init(void);

// 反初始化
void lvgl_ui_deinit(void);

// 获取当前界面状态
ui_state_t lvgl_ui_get_state(void);

// 切换界面状态
void lvgl_ui_set_state(ui_state_t state);

// 运行 LVGL 任务（在主循环或单独任务中调用）
void lvgl_ui_task_handler(void);

// 检查是否应该进入跌倒检测调试
bool lvgl_ui_should_start_fall_debug(void);

// 检查是否应该进入录音界面
bool lvgl_ui_should_start_record(void);

// 检查是否应该返回主界面
bool lvgl_ui_should_return_home(void);

// 设置返回主界面标志
void lvgl_ui_set_return_home(void);

// 检查是否需要暂停跌倒检测（进入录音界面）
bool lvgl_ui_should_pause_detect(void);

// 检查是否需要恢复跌倒检测（退出录音界面）
bool lvgl_ui_should_resume_detect(void);

#endif