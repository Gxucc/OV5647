// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"

// #include "audio_mqtt_sender.h"

// static const char *TAG = "main";

// static void demo_task(void *pv)
// {
//     vTaskDelay(pdMS_TO_TICKS(2000));

//     ESP_LOGI(TAG, "Starting audio MQTT demo...");
//     esp_err_t ret = audio_mqtt_sender_demo(440.0f);
//     if (ret != ESP_OK) {
//         ESP_LOGE(TAG, "Demo failed: %s", esp_err_to_name(ret));
//     }

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }

// void app_main(void)
// {
//     ESP_LOGI(TAG, "App starting...");

//     audio_mqtt_sender_init();

//     xTaskCreate(demo_task, "demo_task", 8192, NULL, 5, NULL);

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }