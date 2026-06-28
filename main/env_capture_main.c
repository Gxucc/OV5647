// #include <stdio.h>
// #include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "driver/i2c_master.h"

// #include "audio_recorder.h"

// static const char *TAG = "env_capture";

// static i2c_master_bus_handle_t s_i2c_bus = NULL;

// void app_main(void)
// {
//     ESP_LOGI(TAG, "=== Environment Sound Capture ===");
//     ESP_LOGI(TAG, "Recording 600 seconds (10 minutes) of environment sound");
//     ESP_LOGI(TAG, "Please keep the environment in its NORMAL state");
//     ESP_LOGI(TAG, "Starting in 5 seconds...");

//     vTaskDelay(pdMS_TO_TICKS(5000));

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

//     audio_recorder_cfg_t cfg = {
//         .sample_rate = 16000,
//         .bits_per_sample = 16,
//         .channel_count = 1,
//         .duration_ms = 6000,  // 10 分钟
//         .save_path = "/sdcard/env_train_long.wav",
//     };

//     ESP_LOGI(TAG, "Recording started...");
//     ESP_ERROR_CHECK(audio_recorder_start(&cfg));

//     // 等待录音完成
//     while (audio_recorder_is_running()) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }

//     ESP_LOGI(TAG, "=== Capture Complete ===");
//     ESP_LOGI(TAG, "File saved to /sdcard/env_train_long.wav");
//     ESP_LOGI(TAG, "Please copy to PC and run split script");

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }