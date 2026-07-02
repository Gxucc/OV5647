#include "audio_mqtt_sender.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "math.h"


static const char *TAG = "audio_mqtt_sender";

// WiFi 配置
#define WIFI_SSID       "Redmi Turbo 3"
#define WIFI_PASSWORD   "00000000"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_mqtt_connected = false;
static volatile bool s_connection_stable = false;
static volatile bool s_connect_failed = false;
static uint16_t s_seq_id = 0;

// 发送状态
static volatile send_status_t s_send_status = SEND_STATUS_IDLE;
static volatile int s_send_progress = 0;

// 连接重试计数
static volatile int s_wifi_retry_count = 0;
static volatile int s_mqtt_retry_count = 0;

// ==================== WiFi 事件处理 ====================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_connect_failed && s_wifi_retry_count < MAX_CONNECT_RETRY && !s_connection_stable) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        s_mqtt_connected = false;
        
        if (s_connection_stable) {
            ESP_LOGW(TAG, "WiFi disconnected (was stable). No auto-reconnect until next send.");
            return;
        }
        
        s_wifi_retry_count++;
        if (s_wifi_retry_count >= MAX_CONNECT_RETRY) {
            ESP_LOGE(TAG, "WiFi connection failed %d times, skipping", MAX_CONNECT_RETRY);
            s_connect_failed = true;
        } else {
            ESP_LOGW(TAG, "WiFi disconnected, retrying... (%d/%d)", s_wifi_retry_count, MAX_CONNECT_RETRY);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wifi_retry_count = 0;
        
        if (!s_connection_stable) {
            s_connection_stable = true;
            ESP_LOGI(TAG, "Connection marked stable. Future disconnects will NOT auto-reconnect.");
        }
    }
}

// ==================== MQTT 事件处理 ====================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        s_mqtt_retry_count = 0;
        ESP_LOGI(TAG, "MQTT connected to broker");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT message published, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error, type=%d", event->error_handle->error_type);
        s_mqtt_retry_count++;
        if (s_mqtt_retry_count >= MAX_CONNECT_RETRY) {
            ESP_LOGE(TAG, "MQTT connection failed %d times, skipping", MAX_CONNECT_RETRY);
            s_connect_failed = true;
        }
        break;
    default:
        break;
    }
}

// ==================== WiFi 初始化 ====================

static esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                      &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_register(WiFi) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                      &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_handler_register(IP) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi init done, connecting to %s...", WIFI_SSID);
    return ESP_OK;
}

// ==================== MQTT 初始化 ====================

static esp_err_t mqtt_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT...");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .session.keepalive = 120,
        .network.timeout_ms = 30000,
        .network.reconnect_timeout_ms = 10000,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                  mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

// ==================== MQTT 发送 PCM 分片 ====================

#define MQTT_MAX_PAYLOAD 4096  //切片大小

static void mqtt_send_pcm_raw(const int16_t *pcm_data, size_t samples)
{
    if (!s_mqtt_connected) {
        ESP_LOGE(TAG, "MQTT not connected, cannot send");
        s_send_status = SEND_STATUS_FAILED;
        return;
    }

    uint32_t seq_id = s_seq_id++;  // 改为 32 位
    size_t pcm_bytes = samples * sizeof(int16_t);
    size_t total_chunks = (pcm_bytes + MQTT_MAX_PAYLOAD - 1) / MQTT_MAX_PAYLOAD;

    ESP_LOGI(TAG, "========== SENDING START ==========");
    ESP_LOGI(TAG, "seq_id=%lu, samples=%d, pcm_bytes=%d, chunks=%d", 
             seq_id, (int)samples, (int)pcm_bytes, (int)total_chunks);

    for (size_t chunk_idx = 0; chunk_idx < total_chunks; chunk_idx++) {
        size_t offset = chunk_idx * MQTT_MAX_PAYLOAD;
        size_t chunk_len = (offset + MQTT_MAX_PAYLOAD > pcm_bytes) ? 
                           (pcm_bytes - offset) : MQTT_MAX_PAYLOAD;
        
        size_t msg_len = 12 + chunk_len;  // 12 字节 header（原来是 8）
        uint8_t *msg = heap_caps_malloc(msg_len, MALLOC_CAP_SPIRAM);
        if (!msg) {
            msg = malloc(msg_len);
        }
        if (!msg) {
            ESP_LOGE(TAG, "Failed to alloc msg buffer for chunk %d", (int)chunk_idx);
            s_send_status = SEND_STATUS_FAILED;
            return;
        }

        // Header (12 bytes) - 修复 uint8_t 溢出
        msg[0] = (seq_id >> 24) & 0xFF;  // seq_id 高字节
        msg[1] = (seq_id >> 16) & 0xFF;
        msg[2] = (seq_id >> 8) & 0xFF;
        msg[3] = seq_id & 0xFF;          // seq_id 低字节
        
        msg[4] = (chunk_idx >> 8) & 0xFF;   // chunk_idx 高字节
        msg[5] = chunk_idx & 0xFF;           // chunk_idx 低字节
        
        msg[6] = (total_chunks >> 8) & 0xFF; // total_chunks 高字节
        msg[7] = total_chunks & 0xFF;        // total_chunks 低字节
        
        msg[8] = (SENDER_SAMPLE_RATE >> 8) & 0xFF;
        msg[9] = SENDER_SAMPLE_RATE & 0xFF;
        
        msg[10] = 1;     // channels
        msg[11] = 0;     // format = 0 表示原始 PCM

        memcpy(msg + 12, (uint8_t*)pcm_data + offset, chunk_len);

        int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_AUDIO,
                                              (char *)msg, msg_len, 0, 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "MQTT publish chunk %d/%d failed", (int)chunk_idx + 1, (int)total_chunks);
            free(msg);
            s_send_status = SEND_STATUS_FAILED;
            return;
        }
        
        if (chunk_idx % 10 == 0 || chunk_idx == total_chunks - 1) {
            ESP_LOGI(TAG, "Published chunk %d/%d, msg_id=%d, payload=%d bytes",
                     (int)chunk_idx + 1, (int)total_chunks, msg_id, (int)chunk_len);
        }

        free(msg);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "========== SENDING COMPLETE ==========");
    ESP_LOGI(TAG, "seq_id=%lu sent successfully in %d chunks", seq_id, (int)total_chunks);
    s_send_status = SEND_STATUS_SUCCESS;
    s_send_progress = 100;
}

// ==================== 读取 WAV 并提取 PCM ====================

static esp_err_t read_wav_pcm(const char *wav_path, int16_t **pcm_out, size_t *samples_out)
{
    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open WAV: %s", wav_path);
        return ESP_FAIL;
    }

    uint8_t wav_header[44];
    if (fread(wav_header, 1, 44, f) != 44) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        fclose(f);
        return ESP_FAIL;
    }

    if (wav_header[0] != 'R' || wav_header[1] != 'I' || 
        wav_header[2] != 'F' || wav_header[3] != 'F') {
        ESP_LOGE(TAG, "Invalid WAV file");
        fclose(f);
        return ESP_FAIL;
    }

    uint32_t data_size = wav_header[40] | (wav_header[41] << 8) | 
                         (wav_header[42] << 16) | (wav_header[43] << 24);
    
    ESP_LOGI(TAG, "WAV data size: %d bytes", data_size);

    size_t samples = data_size / sizeof(int16_t);
    *pcm_out = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);
    if (!*pcm_out) {
        *pcm_out = malloc(data_size);
    }
    if (!*pcm_out) {
        ESP_LOGE(TAG, "Failed to alloc PCM buffer");
        fclose(f);
        return ESP_FAIL;
    }

    size_t read = fread(*pcm_out, 1, data_size, f);
    fclose(f);

    if (read != data_size) {
        ESP_LOGE(TAG, "Failed to read full PCM data: %d/%d", read, data_size);
        free(*pcm_out);
        return ESP_FAIL;
    }

    *samples_out = samples;
    ESP_LOGI(TAG, "Read %d samples from WAV", (int)samples);
    return ESP_OK;
}

// ==================== 公共 API ====================

esp_err_t audio_mqtt_sender_init(void)
{
    ESP_LOGI(TAG, "Audio MQTT Sender initializing...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = wifi_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed, audio send disabled: %s", esp_err_to_name(ret));
        s_connect_failed = true;
        return ret;
    }

    mqtt_init();

    ESP_LOGI(TAG, "Audio MQTT Sender init done, waiting for connection...");
    return ESP_OK;
}

bool audio_mqtt_sender_is_ready(void)
{
    if (s_wifi_connected && s_mqtt_connected) {
        return true;
    }
    
    if (s_connection_stable && !s_wifi_connected && !s_connect_failed) {
        ESP_LOGW(TAG, "Connection stable but lost. Triggering manual reconnect...");
        esp_wifi_connect();
    }
    
    ESP_LOGW(TAG, "Not ready: wifi=%d, mqtt=%d, stable=%d", 
             s_wifi_connected, s_mqtt_connected, s_connection_stable);
    return false;
}

send_status_t audio_mqtt_sender_get_status(void)
{
    return s_send_status;
}

int audio_mqtt_sender_get_progress(void)
{
    return s_send_progress;
}

void audio_mqtt_sender_reset_connection(void)
{
    s_connection_stable = false;
    s_wifi_retry_count = 0;
    s_mqtt_retry_count = 0;
    s_connect_failed = false;
    ESP_LOGI(TAG, "Connection reset. Will attempt full reconnect on next send.");
}

esp_err_t audio_mqtt_sender_send_wav(const char *wav_path)
{
    if (s_connect_failed) {
        ESP_LOGE(TAG, "Connection previously failed, skipping send");
        s_send_status = SEND_STATUS_FAILED;
        return ESP_FAIL;
    }

    if (!audio_mqtt_sender_is_ready()) {
        ESP_LOGE(TAG, "WiFi/MQTT not ready");
        s_send_status = SEND_STATUS_FAILED;
        return ESP_ERR_INVALID_STATE;
    }

    s_send_status = SEND_STATUS_SENDING;
    s_send_progress = 0;

    int16_t *pcm_data = NULL;
    size_t samples = 0;
    esp_err_t ret = read_wav_pcm(wav_path, &pcm_data, &samples);
    if (ret != ESP_OK) {
        s_send_status = SEND_STATUS_FAILED;
        return ret;
    }

    mqtt_send_pcm_raw(pcm_data, samples);
    heap_caps_free(pcm_data);

    return ESP_OK;
}

esp_err_t audio_mqtt_sender_send_pcm(const int16_t *pcm_data, size_t samples)
{
    if (s_connect_failed) {
        ESP_LOGE(TAG, "Connection previously failed, skipping send");
        s_send_status = SEND_STATUS_FAILED;
        return ESP_FAIL;
    }

    if (!audio_mqtt_sender_is_ready()) {
        ESP_LOGE(TAG, "WiFi/MQTT not ready");
        s_send_status = SEND_STATUS_FAILED;
        return ESP_ERR_INVALID_STATE;
    }

    if (!pcm_data || samples == 0) {
        ESP_LOGE(TAG, "Invalid PCM data");
        s_send_status = SEND_STATUS_FAILED;
        return ESP_ERR_INVALID_ARG;
    }

    s_send_status = SEND_STATUS_SENDING;
    s_send_progress = 0;

    ESP_LOGI(TAG, "Starting PCM raw send: %d samples", (int)samples);

    mqtt_send_pcm_raw(pcm_data, samples);

    return ESP_OK;
}

// Demo 函数
esp_err_t audio_mqtt_sender_demo(float freq_hz)
{
    ESP_LOGI(TAG, "========== DEMO START ==========");
    ESP_LOGI(TAG, "Generating %d samples, %d sec @ %d Hz, sine wave @ %.1f Hz",
             SENDER_TOTAL_SAMPLES, SENDER_DURATION_SEC, SENDER_SAMPLE_RATE, freq_hz);

    ESP_LOGI(TAG, "Waiting for WiFi + MQTT connection...");
    int wait_count = 0;
    while (!audio_mqtt_sender_is_ready() && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (!audio_mqtt_sender_is_ready()) {
        ESP_LOGE(TAG, "Connection timeout");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Connected! Starting demo...");

    int16_t *sine_buf = heap_caps_malloc(SENDER_TOTAL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!sine_buf) {
        sine_buf = malloc(SENDER_TOTAL_SAMPLES * sizeof(int16_t));
    }
    if (!sine_buf) {
        ESP_LOGE(TAG, "Failed to alloc sine buffer");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < SENDER_TOTAL_SAMPLES; i++) {
        float t = (float)i / SENDER_SAMPLE_RATE;
        sine_buf[i] = (int16_t)(sinf(2.0f * M_PI * freq_hz * t) * 30000.0f);
    }
    ESP_LOGI(TAG, "Generated %d samples sine wave @ %.1f Hz", SENDER_TOTAL_SAMPLES, freq_hz);

    mqtt_send_pcm_raw(sine_buf, SENDER_TOTAL_SAMPLES);

    heap_caps_free(sine_buf);

    ESP_LOGI(TAG, "========== DEMO END ==========");
    return ESP_OK;
}
esp_mqtt_client_handle_t audio_mqtt_sender_get_client(void)
{
    return s_mqtt_client;
}