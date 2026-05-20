#ifndef NET_SERVICE_H
#define NET_SERVICE_H

#include "esp_err.h"
#include <stdint.h>

// WiFi + HTTP
esp_err_t net_service_wifi_init(void);
esp_err_t net_service_http_start(void);

// 非阻塞更新JPEG（主循环调用，立即返回）
void net_service_jpeg_update(uint8_t *jpeg_buf, uint32_t jpeg_size);

// 从队列获取JPEG（stream_handler内部用）
bool net_service_jpeg_get(uint8_t **jpeg_buf, uint32_t *jpeg_size);

#endif