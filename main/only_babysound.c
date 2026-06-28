// #include <stdio.h>
// #include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "esp_timer.h"
// #include "driver/i2c_master.h"

// #include "audio_recorder.h"
// #include "babysound_detector.h"

// static const char *TAG = "demo";

// #define PCM_SAMPLES     16000
// #define SLIDE_SAMPLES   8000

// static i2c_master_bus_handle_t s_i2c_bus = NULL;

// static int16_t s_pcm_buffer_a[PCM_SAMPLES];
// static int16_t s_pcm_buffer_b[PCM_SAMPLES];
// static int16_t *s_pcm_current = s_pcm_buffer_a;
// static int16_t *s_pcm_next = s_pcm_buffer_b;

// static void babysound_task(void *pv)
// {
//     ESP_LOGI(TAG, "Babysound detection task started");

//     memset(s_pcm_buffer_a, 0, sizeof(s_pcm_buffer_a));
//     memset(s_pcm_buffer_b, 0, sizeof(s_pcm_buffer_b));

//     ESP_LOGI(TAG, "Filling initial 1s buffer...");
//     esp_err_t ret = audio_recorder_read_samples(s_pcm_current, PCM_SAMPLES, 2000);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "Initial read failed: %s", esp_err_to_name(ret));
//         vTaskDelete(NULL);
//         return;
//     }

//     while (1) {
//         babysound_result_t result;
//         ret = babysound_detector_run(s_pcm_current, &result);
//         if (ret == ESP_OK) {
//             ESP_LOGI(TAG, "=== RESULT: %s (baby: %.3f) ===", 
//                      result.label, result.baby_prob);
//         } else {
//             ESP_LOGE(TAG, "Detection failed: %s", esp_err_to_name(ret));
//         }

//         memcpy(s_pcm_next, s_pcm_current + SLIDE_SAMPLES, SLIDE_SAMPLES * sizeof(int16_t));

//         ret = audio_recorder_read_samples(s_pcm_next + SLIDE_SAMPLES, SLIDE_SAMPLES, 1000);
//         if (ret != ESP_OK) {
//             ESP_LOGE(TAG, "Slide read failed: %s", esp_err_to_name(ret));
//             vTaskDelay(pdMS_TO_TICKS(100));
//             continue;
//         }

//         int16_t *tmp = s_pcm_current;
//         s_pcm_current = s_pcm_next;
//         s_pcm_next = tmp;

//         vTaskDelay(pdMS_TO_TICKS(50));
//     }
// }

// void app_main(void)
// {
//     ESP_LOGI(TAG, "=== Babysound Detection Demo ===");

//     i2c_master_bus_config_t i2c_bus_config;
//     memset(&i2c_bus_config, 0, sizeof(i2c_bus_config));
//     i2c_bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
//     i2c_bus_config.glitch_ignore_cnt = 7;
//     i2c_bus_config.i2c_port = I2C_NUM_0;
//     i2c_bus_config.sda_io_num = GPIO_NUM_7;
//     i2c_bus_config.scl_io_num = GPIO_NUM_8;
//     i2c_bus_config.flags.enable_internal_pullup = true;

//     ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus));
//     ESP_ERROR_CHECK(audio_recorder_system_init(s_i2c_bus));
//     ESP_ERROR_CHECK(babysound_detector_init());

//     ESP_LOGI(TAG, "All systems ready, starting detection...");

//     xTaskCreate(babysound_task, "babysound", 8192, NULL, 5, NULL);

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }