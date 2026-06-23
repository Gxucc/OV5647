#ifndef TOUCH_GT911_H
#define TOUCH_GT911_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief 初始化 GT911 触摸芯片
 * @return ESP_OK 成功
 */
esp_err_t touch_gt911_init(void);

/**
 * @brief 读取触摸坐标
 * @param x 输出 X 坐标
 * @param y 输出 Y 坐标
 * @param pressed 输出是否按下
 * @return ESP_OK 成功
 */
esp_err_t touch_gt911_read(int *x, int *y, bool *pressed);

#endif