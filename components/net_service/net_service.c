#include "net_service.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi_remote.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_http_server.h"
#include "cam_net.h"
#include "esp_timer.h"
#include <inttypes.h>

static const char *TAG = "net_service";

/* ========== WiFi 配置 ========== */
#define WIFI_SSID       "Redmi Turbo 3"
#define WIFI_PASS       "00000000"
#define WIFI_MAX_RETRY  5

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int s_retry_num = 0;

/* ========== JPEG 环形队列 ========== */
#define JPEG_QUEUE_SIZE     3

typedef struct {
    uint8_t *buf;
    uint32_t size;
} jpeg_frame_t;

static jpeg_frame_t s_jpeg_queue[JPEG_QUEUE_SIZE];
static volatile int s_queue_head = 0;
static volatile int s_queue_tail = 0;
static SemaphoreHandle_t s_queue_mutex = NULL;

/* ========== WiFi 事件 ========== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect(); s_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ========== HTTP 处理器 ========== */
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html = 
        "<html><head><title>ESP32 Camera</title></head>"
        "<body style='margin:0;background:#000;display:flex;justify-content:center;align-items:center;height:100vh;'>"
        "<img id='stream' src='/stream' style='max-width:100%;max-height:100%;'>"
        "<script>"
        "const img=document.getElementById('stream');"
        "let last=Date.now();"
        "img.onload=function(){last=Date.now();};"
        "setInterval(function(){"
        "  if(Date.now()-last>3000){"
        "    img.src='/stream?t='+Date.now();"
        "    last=Date.now();"
        "  }"
        "},1000);"
        "</script>"
        "</body></html>";
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    uint8_t *jpeg_buf = NULL;
    uint32_t jpeg_size = 0;

    if (net_service_jpeg_get(&jpeg_buf, &jpeg_size)) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_send(req, (const char *)jpeg_buf, jpeg_size);
        return ESP_OK;
    }

    httpd_resp_send_500(req);
    return ESP_FAIL;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t ret;
    uint8_t *jpeg_buf = NULL;
    uint32_t jpeg_size = 0;
    int64_t t_start;

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    while (1) {
        t_start = esp_timer_get_time();

        // 非阻塞取帧
        if (!net_service_jpeg_get(&jpeg_buf, &jpeg_size)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        char part_hdr[64];
        int hdr_len = snprintf(part_hdr, sizeof(part_hdr),
                               "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\n\r\n",
                               jpeg_size);
        ret = httpd_resp_send_chunk(req, part_hdr, hdr_len);
        if (ret != ESP_OK) break;

        ret = httpd_resp_send_chunk(req, (const char *)jpeg_buf, jpeg_size);
        if (ret != ESP_OK) break;

        // 删除 delay！帧率由编码速度和网络决定
        ESP_LOGD(TAG, "wifi_send_time=%lldus", esp_timer_get_time() - t_start);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ========== 队列操作（非阻塞） ========== */
void net_service_jpeg_update(uint8_t *jpeg_buf, uint32_t jpeg_size)
{
    if (!s_queue_mutex || !jpeg_buf || jpeg_size == 0) return;

    // 非阻塞取锁，失败丢帧保 LCD
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1)) != pdTRUE) {
        ESP_LOGW(TAG, "JPEG update skipped, mutex busy");
        return;
    }

    int next = (s_queue_tail + 1) % JPEG_QUEUE_SIZE;
    
    if (next == s_queue_head) {
        s_queue_head = (s_queue_head + 1) % JPEG_QUEUE_SIZE;
    }

    s_jpeg_queue[s_queue_tail].buf = jpeg_buf;
    s_jpeg_queue[s_queue_tail].size = jpeg_size;
    s_queue_tail = next;

    xSemaphoreGive(s_queue_mutex);
}

bool net_service_jpeg_get(uint8_t **jpeg_buf, uint32_t *jpeg_size)
{
    if (!s_queue_mutex) return false;

    // 非阻塞
    if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1)) != pdTRUE) {
        return false;
    }

    if (s_queue_head == s_queue_tail) {
        xSemaphoreGive(s_queue_mutex);
        return false;
    }

    *jpeg_buf = s_jpeg_queue[s_queue_head].buf;
    *jpeg_size = s_jpeg_queue[s_queue_head].size;
    s_queue_head = (s_queue_head + 1) % JPEG_QUEUE_SIZE;

    xSemaphoreGive(s_queue_mutex);
    return true;
}

/* ========== 公开 API ========== */
esp_err_t net_service_wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRY);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi connected");
    return ESP_OK;
}

esp_err_t net_service_http_start(void)
{
    s_queue_mutex = xSemaphoreCreateMutex();
    if (!s_queue_mutex) {
        ESP_LOGE(TAG, "Failed to create queue mutex");
        return ESP_FAIL;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.task_priority = 2;  // ← 从默认 5 降到 2，低于主循环

    httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &capture_uri);
        httpd_register_uri_handler(server, &stream_uri);
        ESP_LOGI(TAG, "HTTP server on port %d", config.server_port);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}