// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/queue.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "esp_timer.h"
// #include "driver/i2c_master.h"
// #include "esp_heap_caps.h"

// #include "lcd_display.h"
// #include "lvgl_ui.h"
// #include "cam_net.h"
// #include "person_detect.h"
// #include "fall_classifier.h"
// #include "audio_recorder.h"
// #include "babysound_detector.h"

// static const char *TAG = "main";

// #define CAM_WIDTH   1024
// #define CAM_HEIGHT  600
// #define FALL_THRESHOLD 0.5f

// typedef struct {
//     uint8_t *rgb_buf;
//     uint32_t rgb_size;
// } cam_frame_t;

// typedef struct {
//     person_box_t box;
//     bool detected;
// } detect_result_t;

// typedef struct {
//     uint8_t *rgb_buf;
//     fall_classifier_box_t box;
//     bool detected;
// } fall_detect_task_t;

// static int s_frame_count = 0;
// static int s_detect_count = 0;
// static int64_t s_last_fps_time = 0;

// static QueueHandle_t s_detect_queue = NULL;
// static QueueHandle_t s_result_queue = NULL;
// static QueueHandle_t s_fall_queue = NULL;
// static QueueHandle_t s_fall_result_queue = NULL;

// static detect_result_t s_prev_result = {0};
// static fall_classifier_result_t s_prev_fall_result = {0};
// static bool s_has_new_result = false;
// static bool s_has_new_fall_result = false;

// static i2c_master_bus_handle_t s_i2c_bus = NULL;
// static TaskHandle_t s_detect_task_handle = NULL;
// static TaskHandle_t s_fall_classify_task_handle = NULL;
// static TaskHandle_t s_babysound_task_handle = NULL;

// #define PCM_SAMPLES         16000
// #define READ_CHUNK_MS       100
// #define READ_CHUNK_SAMPLES  (16000 * READ_CHUNK_MS / 1000)
// #define PCM_RING_SIZE       (PCM_SAMPLES * 2)

// #define MA_WINDOW_SIZE      5
// #define MA_BABY_THRESHOLD   0.6f
// #define MA_HYST_COUNT       3

// static int16_t *s_pcm_ring_buffer = NULL;
// static int16_t *s_pcm_infer_buffer = NULL;

// static int s_pcm_write_idx = 0;
// static float s_ma_window[MA_WINDOW_SIZE];
// static int s_ma_idx = 0;
// static int s_ma_count = 0;

// static float moving_average_update(float new_val)
// {
//     s_ma_window[s_ma_idx] = new_val;
//     s_ma_idx = (s_ma_idx + 1) % MA_WINDOW_SIZE;
//     if (s_ma_count < MA_WINDOW_SIZE) {
//         s_ma_count++;
//     }

//     float sum = 0.0f;
//     for (int i = 0; i < s_ma_count; i++) {
//         sum += s_ma_window[i];
//     }
//     return sum / s_ma_count;
// }

// static void babysound_task(void *pv)
// {
//     ESP_LOGI(TAG, "Babysound detection task started (1s interval, no overlap)");

//     if (!s_pcm_ring_buffer || !s_pcm_infer_buffer) {
//         ESP_LOGE(TAG, "Audio buffers not allocated!");
//         vTaskDelete(NULL);
//         return;
//     }

//     memset(s_pcm_ring_buffer, 0, PCM_RING_SIZE * sizeof(int16_t));
//     s_pcm_write_idx = 0;
//     memset(s_ma_window, 0, sizeof(s_ma_window));
//     s_ma_idx = 0;
//     s_ma_count = 0;

//     int16_t chunk[READ_CHUNK_SAMPLES];
//     int chunk_counter = 0;

//     int baby_state_count = 0;
//     int nonbaby_state_count = 0;
//     bool is_baby_state = false;

//     while (1) {
//         // 开关关闭时休眠
//         if (!lvgl_ui_babysound_switch_is_on()) {
//             vTaskDelay(pdMS_TO_TICKS(100));
//             chunk_counter = 0;
//             continue;
//         }

//         // 进入录音界面时暂停
//         if (lvgl_ui_get_state() == UI_STATE_RECORD) {
//             vTaskDelay(pdMS_TO_TICKS(200));
//             chunk_counter = 0;  // 重置，避免退出后立刻推理
//             continue;
//         }

//         // 读取 100ms chunk
//         esp_err_t ret = audio_recorder_read_samples(chunk, READ_CHUNK_SAMPLES, 300);
//         if (ret != ESP_OK) {
//             ESP_LOGW(TAG, "Chunk read failed: %s", esp_err_to_name(ret));
//             vTaskDelay(pdMS_TO_TICKS(10));
//             continue;
//         }

//         // 写入环形缓冲区
//         for (int i = 0; i < READ_CHUNK_SAMPLES; i++) {
//             s_pcm_ring_buffer[s_pcm_write_idx] = chunk[i];
//             s_pcm_write_idx = (s_pcm_write_idx + 1) % PCM_RING_SIZE;
//         }

//         chunk_counter++;

//         // 每 10 个 chunk（1秒）推理一次
//         if (chunk_counter < 10) {
//             vTaskDelay(pdMS_TO_TICKS(1));
//             continue;
//         }
//         chunk_counter = 0;

//         // 提取最近 1秒数据
//         int start_idx = (s_pcm_write_idx - PCM_SAMPLES + PCM_RING_SIZE) % PCM_RING_SIZE;
//         for (int i = 0; i < PCM_SAMPLES; i++) {
//             s_pcm_infer_buffer[i] = s_pcm_ring_buffer[(start_idx + i) % PCM_RING_SIZE];
//         }

//         // 推理
//         babysound_result_t result;
//         ret = babysound_detector_run(s_pcm_infer_buffer, &result);
//         if (ret != ESP_OK) {
//             ESP_LOGE(TAG, "Babysound detection failed: %s", esp_err_to_name(ret));
//             taskYIELD();
//             continue;
//         }

//         // 滑动窗口平均滤波
//         float ma_baby_prob = moving_average_update(result.baby_prob);

//         // 状态机防抖
//         if (ma_baby_prob > MA_BABY_THRESHOLD) {
//             baby_state_count++;
//             nonbaby_state_count = 0;
//             if (baby_state_count >= MA_HYST_COUNT && !is_baby_state) {
//                 is_baby_state = true;
//                 ESP_LOGI(TAG, "=== BABYSOUND STATE: BABY (raw: %.3f, ma: %.3f) ===",
//                          result.baby_prob, ma_baby_prob);
//             }
//         } else {
//             nonbaby_state_count++;
//             baby_state_count = 0;
//             if (nonbaby_state_count >= MA_HYST_COUNT && is_baby_state) {
//                 is_baby_state = false;
//                 ESP_LOGI(TAG, "=== BABYSOUND STATE: NON-BABY (raw: %.3f, ma: %.3f) ===",
//                          result.baby_prob, ma_baby_prob);
//             }
//         }

//         ESP_LOGI(TAG, "Babysound raw: %.3f, MA: %.3f, state: %s",
//                  result.baby_prob, ma_baby_prob, is_baby_state ? "BABY" : "NON-BABY");

//         taskYIELD();
//     }
// }

// static void detect_task(void *pv)
// {
//     cam_frame_t frame;
//     detect_result_t result;

//     ESP_LOGI(TAG, "detect_task started");

//     while (1) {
//         ESP_LOGD(TAG, "detect_task waiting for frame...");
//         if (xQueueReceive(s_detect_queue, &frame, portMAX_DELAY) == pdTRUE) {
//             ESP_LOGD(TAG, "detect_task received frame, running person_detect...");
//             result.detected = person_detect_run(frame.rgb_buf, CAM_WIDTH, CAM_HEIGHT, &result.box);
//             ESP_LOGD(TAG, "detect_task done, detected=%d", result.detected);
//             xQueueSend(s_result_queue, &result, portMAX_DELAY);
//             cam_net_release_rgb565();
//         }
//     }
// }

// static void fall_classify_task(void *pv)
// {
//     fall_detect_task_t task;
//     fall_classifier_result_t result;

//     ESP_LOGI(TAG, "fall_classify_task started");

//     while (1) {
//         ESP_LOGD(TAG, "fall_classify_task waiting for task...");
//         if (xQueueReceive(s_fall_queue, &task, portMAX_DELAY) == pdTRUE) {
//             ESP_LOGD(TAG, "fall_classify_task received task, detected=%d", task.detected);
//             if (task.detected) {
//                 bool ok = fall_classifier_run(task.rgb_buf, CAM_WIDTH, CAM_HEIGHT, &task.box, &result);
//                 if (ok) {
//                     xQueueSend(s_fall_result_queue, &result, portMAX_DELAY);
//                 }
//                 cam_net_release_rgb565();
//             }
//         }
//     }
// }

// static void display_task(void *pv)
// {
//     int frame_counter = 0;
//     s_last_fps_time = esp_timer_get_time();

//     ESP_LOGI(TAG, "display_task started");

//     while (1) {
//         uint8_t *rgb_buf = NULL;
//         uint32_t rgb_size = 0;

//         if (cam_net_get_rgb565(&rgb_buf, &rgb_size) != ESP_OK) {
//             vTaskDelay(pdMS_TO_TICKS(10));
//             continue;
//         }

//         detect_result_t result;
//         if (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
//             s_prev_result = result;
//             s_has_new_result = true;
//         }

//         fall_classifier_result_t fall_result;
//         if (xQueueReceive(s_fall_result_queue, &fall_result, 0) == pdTRUE) {
//             s_prev_fall_result = fall_result;
//             s_has_new_fall_result = true;
//         }

//         ui_state_t state = lvgl_ui_get_state();

//         if (state == UI_STATE_FALL_DEBUG) {
//             uint8_t *lcd_buf = lcd_display_get_buffer();
//             lcd_display_copy_camera(rgb_buf, CAM_WIDTH, CAM_HEIGHT);

//             if (s_prev_result.detected) {
//                 uint16_t *pixel_buf = (uint16_t *)lcd_buf;
//                 int x1 = s_prev_result.box.x;
//                 int y1 = s_prev_result.box.y;
//                 int x2 = s_prev_result.box.x + s_prev_result.box.width;
//                 int y2 = s_prev_result.box.y + s_prev_result.box.height;

//                 if (x1 < 0) x1 = 0;
//                 if (y1 < 0) y1 = 0;
//                 if (x2 >= LCD_WIDTH) x2 = LCD_WIDTH - 1;
//                 if (y2 >= LCD_HEIGHT) y2 = LCD_HEIGHT - 1;

//                 uint16_t box_color = 0x07E0;
//                 if (s_has_new_fall_result &&
//                     s_prev_fall_result.predicted_class == CLASS_FALL &&
//                     s_prev_fall_result.fall_prob > FALL_THRESHOLD) {
//                     box_color = 0xF800;
//                 }

//                 for (int t = 0; t < 3; t++) {
//                     for (int x = x1; x <= x2; x++) {
//                         if (x >= 0 && x < LCD_WIDTH) {
//                             pixel_buf[(y1 + t) * LCD_WIDTH + x] = box_color;
//                             pixel_buf[(y2 - t) * LCD_WIDTH + x] = box_color;
//                         }
//                     }
//                     for (int y = y1; y <= y2; y++) {
//                         if (y >= 0 && y < LCD_HEIGHT) {
//                             pixel_buf[y * LCD_WIDTH + (x1 + t)] = box_color;
//                             pixel_buf[y * LCD_WIDTH + (x2 - t)] = box_color;
//                         }
//                     }
//                 }
//             }

//             lcd_display_flush();
//         }

//         if (state != UI_STATE_RECORD && frame_counter % 3 == 0) {
//             ESP_LOGD(TAG, "Sending frame to detect queue, frame_counter=%d, state=%d", frame_counter, state);
//             cam_frame_t frame = { .rgb_buf = rgb_buf, .rgb_size = rgb_size };
//             if (xQueueSend(s_detect_queue, &frame, 0) != pdPASS) {
//                 ESP_LOGW(TAG, "Detect queue full!");
//                 cam_net_release_rgb565();
//             }

//             if (s_prev_result.detected) {
//                 fall_detect_task_t fall_task;
//                 fall_task.rgb_buf = rgb_buf;
//                 fall_task.box.x = s_prev_result.box.x;
//                 fall_task.box.y = s_prev_result.box.y;
//                 fall_task.box.width = s_prev_result.box.width;
//                 fall_task.box.height = s_prev_result.box.height;
//                 fall_task.detected = true;
//                 xQueueSend(s_fall_queue, &fall_task, 0);
//             }
//         } else {
//             cam_net_release_rgb565();
//         }
//         frame_counter++;

//         s_frame_count++;
//         if (s_prev_result.detected) {
//             s_detect_count++;
//         }

//         int64_t now = esp_timer_get_time();
//         if (now - s_last_fps_time >= 1000000) {
//             ESP_LOGI(TAG, "FPS: %d, Detected: %d", s_frame_count, s_detect_count);
//             s_frame_count = 0;
//             s_detect_count = 0;
//             s_last_fps_time = now;
//         }

//         vTaskDelay(pdMS_TO_TICKS(1));
//     }
// }

// static void lvgl_task(void *pv)
// {
//     while (1) {
//         if (lvgl_ui_should_start_fall_debug()) {
//             lvgl_ui_set_state(UI_STATE_FALL_DEBUG);
//         }
//         if (lvgl_ui_should_start_record()) {
//             lvgl_ui_set_state(UI_STATE_RECORD);
//         }
//         if (lvgl_ui_should_start_babysound()) {
//             lvgl_ui_set_state(UI_STATE_BABYSOUND);
//         }

//         lvgl_ui_task_handler();
//         vTaskDelay(pdMS_TO_TICKS(5));
//     }
// }

// void app_main(void)
// {
//     ESP_LOGI(TAG, "Starting app...");

//     s_pcm_ring_buffer = (int16_t *)heap_caps_malloc(PCM_RING_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
//     s_pcm_infer_buffer = (int16_t *)heap_caps_malloc(PCM_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
//     if (!s_pcm_ring_buffer || !s_pcm_infer_buffer) {
//         ESP_LOGE(TAG, "Failed to allocate audio buffers in PSRAM!");
//         return;
//     }
//     ESP_LOGI(TAG, "Audio buffers allocated in PSRAM: ring=%p, infer=%p", s_pcm_ring_buffer, s_pcm_infer_buffer);

//     ESP_ERROR_CHECK(cam_net_init());
//     ESP_ERROR_CHECK(i2c_master_get_bus_handle(I2C_NUM_0, &s_i2c_bus));
//     ESP_ERROR_CHECK(audio_recorder_system_init(s_i2c_bus));
//     ESP_ERROR_CHECK(lcd_display_init());
//     ESP_ERROR_CHECK(lvgl_ui_init());

//     ESP_ERROR_CHECK(person_detect_init());
//     if (!fall_classifier_init()) {
//         ESP_LOGE(TAG, "Fall classifier init failed");
//         return;
//     }
//     ESP_ERROR_CHECK(babysound_detector_init());
//     ESP_LOGI(TAG, "AI models loaded");

//     // 启动统一音频采集
//     ESP_ERROR_CHECK(audio_capture_start());
//     ESP_LOGI(TAG, "Audio capture started");

//     ESP_LOGI(TAG, "All modules initialized");

//     s_detect_queue = xQueueCreate(2, sizeof(cam_frame_t));
//     s_result_queue = xQueueCreate(2, sizeof(detect_result_t));
//     s_fall_queue = xQueueCreate(2, sizeof(fall_detect_task_t));
//     s_fall_result_queue = xQueueCreate(2, sizeof(fall_classifier_result_t));

//     xTaskCreatePinnedToCore(detect_task, "detect", 8192, NULL, 3, &s_detect_task_handle, 1);
//     xTaskCreatePinnedToCore(fall_classify_task, "fall_classify", 8192, NULL, 2, &s_fall_classify_task_handle, 1);
//     xTaskCreatePinnedToCore(babysound_task, "babysound", 8192, NULL, 1, &s_babysound_task_handle, 1);
//     xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 5, NULL, 0);
//     xTaskCreatePinnedToCore(lvgl_task, "lvgl", 4096, NULL, 4, NULL, 0);

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }