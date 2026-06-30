#include "audio_mqtt_sender.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
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

#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"
#include "esp_adpcm_enc.h"

static const char *TAG = "audio_mqtt_sender";

// WiFi 配置
#define WIFI_SSID       "Redmi Turbo 3"
#define WIFI_PASSWORD   "00000000"

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_wifi_connected = false;
static bool s_mqtt_connected = false;
static uint16_t s_seq_id = 0;

// ==================== WiFi 事件处理 ====================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
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
        break;
    default:
        break;
    }
}

// ==================== WiFi 初始化 ====================

static esp_err_t wifi_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done, connecting to %s...", WIFI_SSID);
    return ESP_OK;
}

// ==================== MQTT 初始化 ====================

static esp_err_t mqtt_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT...");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .session.keepalive = 60,
        .network.timeout_ms = 10000,
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

// ==================== 正弦波生成 ====================

static void generate_sine_wave(int16_t *buffer, size_t samples, float freq_hz)
{
    for (size_t i = 0; i < samples; i++) {
        float t = (float)i / SENDER_SAMPLE_RATE;
        buffer[i] = (int16_t)(sinf(2.0f * M_PI * freq_hz * t) * 30000.0f);
    }
    ESP_LOGI(TAG, "Generated %d samples sine wave @ %.1f Hz", (int)samples, freq_hz);
}

// ==================== ADPCM 编码 ====================

static esp_err_t adpcm_encode(const int16_t *pcm_in, size_t samples,
                               uint8_t **adpcm_out, size_t *adpcm_len)
{
    esp_audio_err_t ret;
    esp_audio_enc_handle_t enc_handle = NULL;

    // 注册默认编码器
    ret = esp_audio_enc_register_default();
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "ADPCM enc register default failed: %d", ret);
        return ESP_FAIL;
    }

    // 配置 ADPCM 编码器
    esp_adpcm_enc_config_t adpcm_cfg = ESP_ADPCM_ENC_CONFIG_DEFAULT();
    adpcm_cfg.sample_rate = SENDER_SAMPLE_RATE;
    adpcm_cfg.channel = 1;
    adpcm_cfg.bits_per_sample = 16;

    esp_audio_enc_config_t cfg = {
        .type = ESP_AUDIO_TYPE_ADPCM,
        .cfg = &adpcm_cfg,
        .cfg_sz = sizeof(esp_adpcm_enc_config_t),
    };

    ret = esp_audio_enc_open(&cfg, &enc_handle);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "ADPCM enc open failed: %d", ret);
        return ESP_FAIL;
    }

    int in_frame_size = 0, out_frame_size = 0;
    esp_audio_enc_get_frame_size(enc_handle, &in_frame_size, &out_frame_size);
    ESP_LOGI(TAG, "ADPCM frame: in=%d bytes, out=%d bytes", in_frame_size, out_frame_size);

    uint8_t *inbuf = calloc(1, in_frame_size);
    uint8_t *outbuf = calloc(1, out_frame_size);
    if (!inbuf || !outbuf) {
        ESP_LOGE(TAG, "Buffer alloc failed");
        goto cleanup;
    }

    size_t max_adpcm = (samples / 2) + out_frame_size + 1024;
    *adpcm_out = heap_caps_malloc(max_adpcm, MALLOC_CAP_SPIRAM);
    if (!*adpcm_out) {
        *adpcm_out = malloc(max_adpcm);
    }
    if (!*adpcm_out) {
        ESP_LOGE(TAG, "ADPCM output alloc failed");
        goto cleanup;
    }

    size_t offset = 0;
    size_t pcm_offset = 0;

    while (pcm_offset < samples) {
        size_t to_copy = in_frame_size / sizeof(int16_t);
        if (pcm_offset + to_copy > samples) {
            to_copy = samples - pcm_offset;
        }

        memcpy(inbuf, &pcm_in[pcm_offset], to_copy * sizeof(int16_t));
        if (to_copy < in_frame_size / sizeof(int16_t)) {
            memset(inbuf + to_copy * sizeof(int16_t), 0,
                   in_frame_size - to_copy * sizeof(int16_t));
        }

        esp_audio_enc_in_frame_t in_frame = {
            .buffer = inbuf,
            .len = in_frame_size,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = outbuf,
            .len = out_frame_size,
        };

        ret = esp_audio_enc_process(enc_handle, &in_frame, &out_frame);
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Encode process failed: %d", ret);
            break;
        }

        if (offset + out_frame.encoded_bytes > max_adpcm) {
            ESP_LOGE(TAG, "ADPCM buffer overflow");
            break;
        }

        memcpy(*adpcm_out + offset, outbuf, out_frame.encoded_bytes);
        offset += out_frame.encoded_bytes;
        pcm_offset += to_copy;
    }

    *adpcm_len = offset;
    ESP_LOGI(TAG, "ADPCM encode: %d samples -> %d bytes (ratio %.2f:1)",
             (int)samples, (int)offset, (float)(samples * 2) / offset);

cleanup:
    free(inbuf);
    free(outbuf);
    esp_audio_enc_close(enc_handle);

    return (*adpcm_len > 0) ? ESP_OK : ESP_FAIL;
}

// ==================== MQTT 发送 ADPCM 分片 ====================

static void mqtt_send_adpcm(uint8_t *adpcm_data, size_t adpcm_len)
{
    if (!s_mqtt_connected) {
        ESP_LOGE(TAG, "MQTT not connected, cannot send");
        return;
    }

    uint16_t seq_id = s_seq_id++;
    size_t total_chunks = (adpcm_len + SENDER_CHUNK_SIZE - 1) / SENDER_CHUNK_SIZE;

    ESP_LOGI(TAG, "========== SENDING START ==========");
    ESP_LOGI(TAG, "seq_id=%d, total_chunks=%d, adpcm_len=%d bytes",
             seq_id, (int)total_chunks, (int)adpcm_len);

    for (size_t i = 0; i < total_chunks; i++) {
        size_t chunk_start = i * SENDER_CHUNK_SIZE;
        size_t chunk_len = (i + 1 == total_chunks) ?
                           (adpcm_len - chunk_start) : SENDER_CHUNK_SIZE;

        size_t msg_len = 8 + chunk_len;
        uint8_t *msg = heap_caps_malloc(msg_len, MALLOC_CAP_SPIRAM);
        if (!msg) {
            msg = malloc(msg_len);
        }
        if (!msg) {
            ESP_LOGE(TAG, "Failed to alloc msg buffer");
            return;
        }

        msg[0] = (seq_id >> 8) & 0xFF;
        msg[1] = seq_id & 0xFF;
        msg[2] = (uint8_t)i;
        msg[3] = (uint8_t)total_chunks;
        msg[4] = (SENDER_SAMPLE_RATE >> 8) & 0xFF;
        msg[5] = SENDER_SAMPLE_RATE & 0xFF;
        msg[6] = 1;
        msg[7] = 0;

        memcpy(msg + 8, adpcm_data + chunk_start, chunk_len);

        int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_AUDIO,
                                              (char *)msg, msg_len, 0, 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "MQTT publish failed chunk %d/%d", (int)i, (int)total_chunks);
        } else {
            ESP_LOGI(TAG, "Published chunk %d/%d, msg_id=%d, payload=%d bytes",
                     (int)i, (int)total_chunks, msg_id, (int)chunk_len);
        }

        free(msg);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "========== SENDING COMPLETE ==========");
    ESP_LOGI(TAG, "seq_id=%d sent successfully", seq_id);
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
    ESP_ERROR_CHECK(ret);

    wifi_init();
    mqtt_init();

    ESP_LOGI(TAG, "Audio MQTT Sender init done, waiting for connection...");
    return ESP_OK;
}

bool audio_mqtt_sender_is_ready(void)
{
    return s_wifi_connected && s_mqtt_connected;
}

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

    generate_sine_wave(sine_buf, SENDER_TOTAL_SAMPLES, freq_hz);

    uint8_t *adpcm_data = NULL;
    size_t adpcm_len = 0;
    ESP_ERROR_CHECK(adpcm_encode(sine_buf, SENDER_TOTAL_SAMPLES, &adpcm_data, &adpcm_len));

    mqtt_send_adpcm(adpcm_data, adpcm_len);

    heap_caps_free(sine_buf);
    heap_caps_free(adpcm_data);

    ESP_LOGI(TAG, "========== DEMO END ==========");
    return ESP_OK;
}