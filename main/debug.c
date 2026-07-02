// #include <stdio.h>
// #include <string.h>
// #include <math.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "esp_log.h"
// #include "esp_err.h"
// #include "esp_wifi.h"
// #include "esp_event.h"
// #include "mqtt_client.h"
// #include "nvs_flash.h"
// #include "esp_heap_caps.h"
// #include "esp_timer.h"

// static const char *TAG = "debug_main";

// #define WIFI_SSID       "Redmi Turbo 3"
// #define WIFI_PASSWORD   "00000000"
// #define MQTT_BROKER_URL "mqtt://broker.emqx.io:1883"
// #define MQTT_TOPIC      "homecare/debug/audio"

// #define SAMPLE_RATE     16000
// #define DURATION_SEC    10
// #define TOTAL_SAMPLES   (SAMPLE_RATE * DURATION_SEC)
// #define SINE_FREQ       440.0f  // 440Hz 标准音

// static esp_mqtt_client_handle_t s_client = NULL;
// static volatile bool s_wifi_connected = false;
// static volatile bool s_mqtt_connected = false;

// // ========== WiFi 事件 ==========
// static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
// {
//     if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
//         esp_wifi_connect();
//     } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
//         s_wifi_connected = false;
//         ESP_LOGW(TAG, "WiFi disconnected");
//         esp_wifi_connect();
//     } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
//         s_wifi_connected = true;
//         ESP_LOGI(TAG, "WiFi connected");
//     }
// }

// // ========== MQTT 事件 ==========
// static void mqtt_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
// {
//     esp_mqtt_event_handle_t event = event_data;
//     switch (id) {
//     case MQTT_EVENT_CONNECTED:
//         s_mqtt_connected = true;
//         ESP_LOGI(TAG, "MQTT connected");
//         break;
//     case MQTT_EVENT_DISCONNECTED:
//         s_mqtt_connected = false;
//         ESP_LOGW(TAG, "MQTT disconnected");
//         break;
//     case MQTT_EVENT_PUBLISHED:
//         ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
//         break;
//     case MQTT_EVENT_ERROR:
//         ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle->error_type);
//         break;
//     default:
//         break;
//     }
// }

// // ========== WiFi 初始化 ==========
// static esp_err_t wifi_init(void)
// {
//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());
//     esp_netif_create_default_wifi_sta();

//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_wifi_init(&cfg));
//     ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_handler, NULL));
//     ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_handler, NULL));

//     wifi_config_t wifi_cfg = {
//         .sta = {
//             .ssid = WIFI_SSID,
//             .password = WIFI_PASSWORD,
//             .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
//         },
//     };
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
//     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
//     ESP_ERROR_CHECK(esp_wifi_start());
//     return ESP_OK;
// }

// // ========== MQTT 初始化（大缓冲区） ==========
// static esp_err_t mqtt_init(void)
// {
//     esp_mqtt_client_config_t cfg = {
//         .broker.address.uri = MQTT_BROKER_URL,
//         .session.keepalive = 120,
//         .network.timeout_ms = 30000,
//         .network.reconnect_timeout_ms = 10000,
//         // 尝试设置大缓冲区
//         .buffer.size = 512 * 1024,      // 512KB 接收缓冲区
//         .buffer.out_size = 512 * 1024,  // 512KB 发送缓冲区
//     };

//     s_client = esp_mqtt_client_init(&cfg);
//     if (!s_client) {
//         ESP_LOGE(TAG, "MQTT client init failed");
//         return ESP_FAIL;
//     }

//     esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_handler, NULL);
//     esp_mqtt_client_start(s_client);
//     ESP_LOGI(TAG, "MQTT client started");
//     return ESP_OK;
// }

// // ========== 生成正弦波并发送 ==========
// static void send_sine_wave(void)
// {
//     // 等待连接
//     ESP_LOGI(TAG, "Waiting for WiFi + MQTT...");
//     int wait = 0;
//     while ((!s_wifi_connected || !s_mqtt_connected) && wait < 300) {
//         vTaskDelay(pdMS_TO_TICKS(100));
//         wait++;
//     }

//     if (!s_wifi_connected || !s_mqtt_connected) {
//         ESP_LOGE(TAG, "Connection timeout, abort");
//         return;
//     }

//     // 生成 10 秒正弦波
//     size_t pcm_bytes = TOTAL_SAMPLES * sizeof(int16_t);
//     ESP_LOGI(TAG, "Generating %d samples (%d bytes) sine wave @ %.1f Hz",
//              TOTAL_SAMPLES, pcm_bytes, SINE_FREQ);

//     int16_t *sine_buf = heap_caps_malloc(pcm_bytes, MALLOC_CAP_SPIRAM);
//     if (!sine_buf) {
//         sine_buf = malloc(pcm_bytes);
//     }
//     if (!sine_buf) {
//         ESP_LOGE(TAG, "Failed to alloc sine buffer");
//         return;
//     }

//     for (int i = 0; i < TOTAL_SAMPLES; i++) {
//         float t = (float)i / SAMPLE_RATE;
//         sine_buf[i] = (int16_t)(sinf(2.0f * M_PI * SINE_FREQ * t) * 30000.0f);
//     }

//     // 组装 header + PCM
//     // header: [0-3] magic "SINE" 
//     // [4-7] sample_rate (uint32_t)
//     // [8-11] total_samples (uint32_t)
//     // [12-15] freq_hz (float)
//     // [16+] PCM data
//     size_t msg_len = 16 + pcm_bytes;
//     uint8_t *msg = heap_caps_malloc(msg_len, MALLOC_CAP_SPIRAM);
//     if (!msg) {
//         msg = malloc(msg_len);
//     }
//     if (!msg) {
//         ESP_LOGE(TAG, "Failed to alloc msg buffer");
//         heap_caps_free(sine_buf);
//         return;
//     }

//     memcpy(msg, "SINE", 4);
//     msg[4] = (SAMPLE_RATE >> 24) & 0xFF;
//     msg[5] = (SAMPLE_RATE >> 16) & 0xFF;
//     msg[6] = (SAMPLE_RATE >> 8) & 0xFF;
//     msg[7] = SAMPLE_RATE & 0xFF;
//     msg[8] = (TOTAL_SAMPLES >> 24) & 0xFF;
//     msg[9] = (TOTAL_SAMPLES >> 16) & 0xFF;
//     msg[10] = (TOTAL_SAMPLES >> 8) & 0xFF;
//     msg[11] = TOTAL_SAMPLES & 0xFF;
//     float freq = SINE_FREQ;
//     memcpy(msg + 12, &freq, sizeof(float));
//     memcpy(msg + 16, sine_buf, pcm_bytes);

//     ESP_LOGI(TAG, "Sending %d bytes in ONE message...", msg_len);
//     int msg_id = esp_mqtt_client_publish(s_client, MQTT_TOPIC, (char *)msg, msg_len, 0, 0);
//     if (msg_id < 0) {
//         ESP_LOGE(TAG, "Publish failed: %d", msg_id);
//     } else {
//         ESP_LOGI(TAG, "Publish queued, msg_id=%d", msg_id);
//     }

//     heap_caps_free(msg);
//     heap_caps_free(sine_buf);
// }

// // ========== 主入口 ==========
// void app_main(void)
// {
//     ESP_LOGI(TAG, "=== DEBUG: Send 10s sine wave in ONE message ===");

//     // 打印内存
//     ESP_LOGI(TAG, "Free internal: %d", esp_get_free_internal_heap_size());
//     ESP_LOGI(TAG, "Free total: %d", esp_get_free_heap_size());
//     ESP_LOGI(TAG, "Largest PSRAM block: %d", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

//     // NVS
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }

//     // WiFi + MQTT
//     wifi_init();
//     mqtt_init();

//     // 延迟 3 秒后发送
//     vTaskDelay(pdMS_TO_TICKS(3000));
//     send_sine_wave();

//     // 主循环
//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }