#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 人脸识别结果 */
typedef struct {
    int face_id;           // 人脸ID，-1表示陌生人
    float similarity;      // 相似度 (0.0 ~ 1.0)
    int x;                 // 人脸框坐标
    int y;
    int width;
    int height;
    float detect_score;    // 检测置信度
    bool recognized;       // true: 识别成功, false: 陌生人
} face_recognition_result_t;

/**
 * @brief 初始化人脸识别模块
 * @param threshold 识别阈值，建议 0.6 ~ 0.8
 * @return ESP_OK 成功
 */
esp_err_t face_recognition_init(float threshold);

/**
 * @brief 反初始化，释放资源
 */
void face_recognition_deinit(void);

/**
 * @brief 执行人脸识别（检测 + 识别）
 * @param rgb565_buf RGB565 图像缓冲区
 * @param img_width 图像宽度
 * @param img_height 图像高度
 * @param out_result 输出识别结果
 * @return true: 检测到人脸, false: 未检测到人脸
 */
bool face_recognition_run(const uint8_t *rgb565_buf, int img_width, int img_height,
                          face_recognition_result_t *out_result);

/**
 * @brief 识别 + 在 LCD 缓冲区上画框
 * @param rgb565_buf 摄像头图像
 * @param cam_w 摄像头宽度
 * @param cam_h 摄像头高度
 * @param lcd_buf LCD 显示缓冲区
 * @param lcd_w LCD 宽度
 * @param lcd_h LCD 高度
 * @return true: 检测到人脸
 */
bool face_recognition_process(const uint8_t *rgb565_buf, int cam_w, int cam_h,
                              uint8_t *lcd_buf, int lcd_w, int lcd_h);

#ifdef __cplusplus
}
#endif

#endif