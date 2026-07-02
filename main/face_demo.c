// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/queue.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "esp_timer.h"

// #include "lcd_display.h"
// #include "cam_net.h"
// #include "face_recognition.h"

// static const char *TAG = "main";

// #define CAM_WIDTH   1024
// #define CAM_HEIGHT  600
// #define RECOGNITION_INTERVAL_FRAMES  1  // 每帧都识别

// typedef struct {
//     uint8_t *rgb_buf;
//     uint32_t rgb_size;
// } cam_frame_t;

// typedef struct {
//     face_recognition_result_t result;
//     bool detected;
// } recognition_result_t;

// static QueueHandle_t s_recog_queue = NULL;
// static QueueHandle_t s_result_queue = NULL;

// /* ==================== 人脸识别任务 (Core 1) ==================== */
// static void face_recognition_task(void *pv)
// {
//     cam_frame_t frame;
//     recognition_result_t result;

//     while (1) {
//         if (xQueueReceive(s_recog_queue, &frame, portMAX_DELAY) == pdTRUE) {
//             result.detected = face_recognition_run(frame.rgb_buf, CAM_WIDTH, CAM_HEIGHT, &result.result);
//             xQueueSend(s_result_queue, &result, portMAX_DELAY);
//             cam_net_release_rgb565();
//         }
//     }
// }

// /* ==================== 显示主任务 ==================== */
// static void display_task(void *pv)
// {
//     int frame_count = 0;
//     int face_count = 0;
//     int64_t last_fps_time = esp_timer_get_time();
//     recognition_result_t prev_result = {0};
//     int frame_counter = 0;

//     while (1) {
//         uint8_t *rgb_buf = NULL;
//         uint32_t rgb_size = 0;

//         if (cam_net_get_rgb565(&rgb_buf, &rgb_size) != ESP_OK) {
//             vTaskDelay(pdMS_TO_TICKS(100));
//             continue;
//         }

//         // 获取最新识别结果（非阻塞）
//         recognition_result_t recog_result;
//         if (xQueueReceive(s_result_queue, &recog_result, 0) == pdTRUE) {
//             prev_result = recog_result;
//         }

//         // 拷贝摄像头画面到LCD
//         uint8_t *lcd_buf = lcd_display_get_buffer();
//         lcd_display_copy_camera(rgb_buf, CAM_WIDTH, CAM_HEIGHT);
//         uint16_t *pixel_buf = (uint16_t *)lcd_buf;

//         // 画人脸框
//         if (prev_result.detected) {
//             int x1 = prev_result.result.x;
//             int y1 = prev_result.result.y;
//             int x2 = prev_result.result.x + prev_result.result.width;
//             int y2 = prev_result.result.y + prev_result.result.height;

//             if (x1 < 0) x1 = 0;
//             if (y1 < 0) y1 = 0;
//             if (x2 >= LCD_WIDTH) x2 = LCD_WIDTH - 1;
//             if (y2 >= LCD_HEIGHT) y2 = LCD_HEIGHT - 1;

//             // 识别成功=绿色，陌生人=红色
//             uint16_t box_color = prev_result.result.recognized ? 0x07E0 : 0xF800;

//             for (int t = 0; t < 3; t++) {
//                 for (int x = x1; x <= x2; x++) {
//                     if (x >= 0 && x < LCD_WIDTH) {
//                         pixel_buf[(y1 + t) * LCD_WIDTH + x] = box_color;
//                         pixel_buf[(y2 - t) * LCD_WIDTH + x] = box_color;
//                     }
//                 }
//                 for (int y = y1; y <= y2; y++) {
//                     if (y >= 0 && y < LCD_HEIGHT) {
//                         pixel_buf[y * LCD_WIDTH + (x1 + t)] = box_color;
//                         pixel_buf[y * LCD_WIDTH + (x2 - t)] = box_color;
//                     }
//                 }
//             }

//             // 输出身份和置信度
//             if (prev_result.result.recognized) {
//                 ESP_LOGI(TAG, ">>> [识别成功] 身份ID=%d, 置信度=%.3f",
//                          prev_result.result.face_id, prev_result.result.similarity);
//             } else {
//                 ESP_LOGI(TAG, ">>> [陌生人] 最佳相似度=%.3f", prev_result.result.similarity);
//             }
//             face_count++;
//         }

//         lcd_display_flush();

//         // 每帧都送人脸识别任务
//         if (frame_counter % RECOGNITION_INTERVAL_FRAMES == 0) {
//             cam_frame_t frame = { .rgb_buf = rgb_buf, .rgb_size = rgb_size };
//             if (xQueueSend(s_recog_queue, &frame, 0) != pdPASS) {
//                 cam_net_release_rgb565();  // 队列满，释放帧
//             }
//         } else {
//             cam_net_release_rgb565();
//         }
//         frame_counter++;

//         // FPS统计
//         frame_count++;
//         int64_t now = esp_timer_get_time();
//         if (now - last_fps_time >= 1000000) {
//             ESP_LOGI(TAG, "FPS: %d, Face detected: %d", frame_count, face_count);
//             frame_count = 0;
//             face_count = 0;
//             last_fps_time = now;
//         }

//         vTaskDelay(pdMS_TO_TICKS(1));
//     }
// }

// /* ==================== 主入口 ==================== */
// void app_main(void)
// {
//     ESP_LOGI(TAG, "Starting Face Recognition Demo...");

//     s_recog_queue = xQueueCreate(2, sizeof(cam_frame_t));
//     s_result_queue = xQueueCreate(2, sizeof(recognition_result_t));

//     ESP_ERROR_CHECK(lcd_display_init());
//     ESP_ERROR_CHECK(cam_net_init());
//     ESP_ERROR_CHECK(face_recognition_init(0.65f));

//     ESP_LOGI(TAG, "All modules initialized, face_db in flash");

//     // 人脸识别任务放在 Core 1
//     xTaskCreatePinnedToCore(face_recognition_task, "face_recog", 8192, NULL, 5, NULL, 1);

//     // 显示任务在主线程
//     display_task(NULL);

//     // 清理（实际上不会执行到这里）
//     face_recognition_deinit();
//     lcd_display_deinit();
// }