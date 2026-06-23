#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "lcd_display.h"
#include "lvgl_ui.h"
#include "cam_net.h"
#include "person_detect.h"
#include "fall_classifier.h"

static const char *TAG = "main";

#define CAM_WIDTH   1024
#define CAM_HEIGHT  600

// 跌倒判断阈值
#define FALL_THRESHOLD 0.5f

// ==================== 类型定义 ====================

typedef struct {
    uint8_t *rgb_buf;
    uint32_t rgb_size;
} cam_frame_t;

typedef struct {
    person_box_t box;
    bool detected;
} detect_result_t;

typedef struct {
    uint8_t *rgb_buf;
    fall_classifier_box_t box;
    bool detected;
} fall_detect_task_t;

// ==================== 全局变量 ====================

// 帧率统计
static int s_frame_count = 0;
static int s_detect_count = 0;
static int64_t s_last_fps_time = 0;

// 队列
static QueueHandle_t s_detect_queue = NULL;
static QueueHandle_t s_result_queue = NULL;
static QueueHandle_t s_fall_queue = NULL;
static QueueHandle_t s_fall_result_queue = NULL;

// 最新检测结果（display_task读取）
static detect_result_t s_prev_result = {0};
static fall_classifier_result_t s_prev_fall_result = {0};
static bool s_has_new_result = false;
static bool s_has_new_fall_result = false;

// ==================== 检测任务 ====================

static void detect_task(void *pv)
{
    cam_frame_t frame;
    detect_result_t result;

    while (1) {
        if (xQueueReceive(s_detect_queue, &frame, portMAX_DELAY) == pdTRUE) {
            result.detected = person_detect_run(frame.rgb_buf, CAM_WIDTH, CAM_HEIGHT, &result.box);
            xQueueSend(s_result_queue, &result, portMAX_DELAY);
            cam_net_release_rgb565();
        }
    }
}

static void fall_classify_task(void *pv)
{
    fall_detect_task_t task;
    fall_classifier_result_t result;

    while (1) {
        if (xQueueReceive(s_fall_queue, &task, portMAX_DELAY) == pdTRUE) {
            if (task.detected) {
                bool ok = fall_classifier_run(task.rgb_buf, CAM_WIDTH, CAM_HEIGHT, &task.box, &result);
                if (ok) {
                    xQueueSend(s_fall_result_queue, &result, portMAX_DELAY);
                }
                cam_net_release_rgb565();
            }
        }
    }
}

// ==================== 显示任务 ====================

static void display_task(void *pv)
{
    int frame_counter = 0;
    s_last_fps_time = esp_timer_get_time();

    while (1) {
        uint8_t *rgb_buf = NULL;
        uint32_t rgb_size = 0;

        // 获取摄像头帧
        if (cam_net_get_rgb565(&rgb_buf, &rgb_size) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 获取最新检测结果
        detect_result_t result;
        if (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
            s_prev_result = result;
            s_has_new_result = true;
        }

        // 获取最新跌倒分类结果
        fall_classifier_result_t fall_result;
        if (xQueueReceive(s_fall_result_queue, &fall_result, 0) == pdTRUE) {
            s_prev_fall_result = fall_result;
            s_has_new_fall_result = true;
        }

        // 根据当前UI状态决定显示内容
        ui_state_t state = lvgl_ui_get_state();

        if (state == UI_STATE_FALL_DEBUG) {
            // 调试界面：显示摄像头画面 + 画框
            uint8_t *lcd_buf = lcd_display_get_buffer();
            lcd_display_copy_camera(rgb_buf, CAM_WIDTH, CAM_HEIGHT);

            // 画框和跌倒结果
            if (s_prev_result.detected) {
                uint16_t *pixel_buf = (uint16_t *)lcd_buf;
                int x1 = s_prev_result.box.x;
                int y1 = s_prev_result.box.y;
                int x2 = s_prev_result.box.x + s_prev_result.box.width;
                int y2 = s_prev_result.box.y + s_prev_result.box.height;

                if (x1 < 0) x1 = 0;
                if (y1 < 0) y1 = 0;
                if (x2 >= LCD_WIDTH) x2 = LCD_WIDTH - 1;
                if (y2 >= LCD_HEIGHT) y2 = LCD_HEIGHT - 1;

                // 根据跌倒结果选择颜色：跌倒=红色，正常=绿色
                uint16_t box_color = 0x07E0;  // 默认绿色
                if (s_has_new_fall_result && 
                    s_prev_fall_result.predicted_class == CLASS_FALL && 
                    s_prev_fall_result.fall_prob > FALL_THRESHOLD) {
                    box_color = 0xF800;  // 红色
                }

                for (int t = 0; t < 3; t++) {
                    for (int x = x1; x <= x2; x++) {
                        if (x >= 0 && x < LCD_WIDTH) {
                            pixel_buf[(y1 + t) * LCD_WIDTH + x] = box_color;
                            pixel_buf[(y2 - t) * LCD_WIDTH + x] = box_color;
                        }
                    }
                    for (int y = y1; y <= y2; y++) {
                        if (y >= 0 && y < LCD_HEIGHT) {
                            pixel_buf[y * LCD_WIDTH + (x1 + t)] = box_color;
                            pixel_buf[y * LCD_WIDTH + (x2 - t)] = box_color;
                        }
                    }
                }
            }

            lcd_display_flush();
        }
        // UI_STATE_HOME 时：不显示摄像头画面，只释放帧

        // 隔3帧检测一次，同时送跌倒分类
        if (frame_counter % 3 == 0) {
            cam_frame_t frame = { .rgb_buf = rgb_buf, .rgb_size = rgb_size };
            if (xQueueSend(s_detect_queue, &frame, 0) != pdPASS) {
                cam_net_release_rgb565();
            }

            // 送跌倒分类任务
            if (s_prev_result.detected) {
                fall_detect_task_t fall_task;
                fall_task.rgb_buf = rgb_buf;
                fall_task.box.x = s_prev_result.box.x;
                fall_task.box.y = s_prev_result.box.y;
                fall_task.box.width = s_prev_result.box.width;
                fall_task.box.height = s_prev_result.box.height;
                fall_task.detected = true;
                xQueueSend(s_fall_queue, &fall_task, 0);
            }
        } else {
            cam_net_release_rgb565();
        }
        frame_counter++;

        // FPS 统计
        s_frame_count++;
        if (s_prev_result.detected) s_detect_count++;

        int64_t now = esp_timer_get_time();
        if (now - s_last_fps_time >= 1000000) {
            ESP_LOGI(TAG, "FPS: %d, Detected: %d", s_frame_count, s_detect_count);
            s_frame_count = 0;
            s_detect_count = 0;
            s_last_fps_time = now;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ==================== LVGL 任务 ====================

static void lvgl_task(void *pv)
{
    while (1) {
        // 检查是否需要切换界面
        if (lvgl_ui_should_start_fall_debug()) {
            lvgl_ui_set_state(UI_STATE_FALL_DEBUG);
        }

        lvgl_ui_task_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ==================== 主函数 ====================

void app_main(void)
{
    ESP_LOGI(TAG, "Starting app...");

    // 创建队列
    s_detect_queue = xQueueCreate(2, sizeof(cam_frame_t));
    s_result_queue = xQueueCreate(2, sizeof(detect_result_t));
    s_fall_queue = xQueueCreate(2, sizeof(fall_detect_task_t));
    s_fall_result_queue = xQueueCreate(2, sizeof(fall_classifier_result_t));

    // 初始化模块
    ESP_ERROR_CHECK(cam_net_init());
    ESP_ERROR_CHECK(lcd_display_init());
    ESP_ERROR_CHECK(lvgl_ui_init());
    ESP_ERROR_CHECK(person_detect_init());

    if (!fall_classifier_init()) {
        ESP_LOGE(TAG, "Fall classifier init failed");
        return;
    }

    ESP_LOGI(TAG, "All modules initialized");

    // 创建任务
    xTaskCreatePinnedToCore(detect_task, "detect", 8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(fall_classify_task, "fall_classify", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 4096, NULL, 2, NULL, 0);

    // 主循环（空转，任务已分配）
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}