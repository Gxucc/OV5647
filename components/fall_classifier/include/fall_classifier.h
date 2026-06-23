/**
 * @file fall_classifier.h
 * @brief ESP32-P4 跌倒分类器 C接口（胶水层）
 * 
 * 使用流程:
 * 1. fall_classifier_init() - 初始化，自动加载模型
 * 2. fall_classifier_run() - 对单个人体框进行跌倒分类
 * 3. fall_classifier_deinit() - 释放资源
 * 
 * 依赖: ESP-DL >= 3.3.1
 */

#ifndef FALL_CLASSIFIER_H
#define FALL_CLASSIFIER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FALL_CLASSIFIER_TAG "fall_classifier"
#define FALL_CLASS_INPUT_SIZE 64
#define FALL_CLASS_NUM_CLASSES 2

// 类别索引
#define CLASS_FALL   0
#define CLASS_NORMAL 1

// 默认跌倒判断阈值
#define FALL_CONFIDENCE_THRESHOLD 0.5f

/**
 * @brief 跌倒分类结果
 */
typedef struct {
    float fall_prob;      // 跌倒概率 (0.0 ~ 1.0)
    float normal_prob;    // 正常概率 (0.0 ~ 1.0)
    int predicted_class;  // 0=fall, 1=normal
    const char* label;    // "FALL" 或 "NORMAL"
} fall_classifier_result_t;

/**
 * @brief 人体框坐标
 */
typedef struct {
    int x;          // 左上角X
    int y;          // 左上角Y
    int width;      // 框宽度
    int height;     // 框高度
} fall_classifier_box_t;

/**
 * @brief 初始化跌倒分类器
 * @return true 成功, false 失败
 * 
 * 自动从Flash加载 /spiffs/model/fall_classifier.espdl 模型文件
 */
bool fall_classifier_init(void);

/**
 * @brief 释放跌倒分类器资源
 */
void fall_classifier_deinit(void);

/**
 * @brief 对单个人体框进行跌倒分类
 * 
 * @param rgb565_buf 原始RGB565图像数据
 * @param img_width 原始图像宽度
 * @param img_height 原始图像高度
 * @param box 人体框坐标
 * @param result 输出分类结果
 * @return true 成功, false 失败
 */
bool fall_classifier_run(const uint8_t *rgb565_buf,
                         int img_width,
                         int img_height,
                         const fall_classifier_box_t *box,
                         fall_classifier_result_t *result);

/**
 * @brief 快速判断是否为跌倒
 * 
 * @param rgb565_buf 原始RGB565图像数据
 * @param img_width 原始图像宽度
 * @param img_height 原始图像高度
 * @param box 人体框坐标
 * @param threshold 置信度阈值 (默认0.5)
 * @return true 跌倒, false 正常或失败
 */
bool fall_classifier_is_fall(const uint8_t *rgb565_buf,
                               int img_width,
                               int img_height,
                               const fall_classifier_box_t *box,
                               float threshold);

#ifdef __cplusplus
}
#endif

#endif // FALL_CLASSIFIER_H