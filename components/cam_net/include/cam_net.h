#ifndef CAM_NET_H
#define CAM_NET_H

#include <stdint.h>
#include "esp_err.h"

// 原有 API
esp_err_t cam_net_init(void);
esp_err_t cam_net_get_jpeg(uint8_t **buf, uint32_t *size);

// 新增：从已有 RGB565 缓冲编码 JPEG（不操作 V4L2）
esp_err_t cam_net_encode_jpeg(uint8_t *rgb565_buf, uint32_t rgb_size,
                               uint8_t **jpeg_buf, uint32_t *jpeg_size);

// ISP 控制
esp_err_t cam_net_isp_set_brightness(int val);
esp_err_t cam_net_isp_set_contrast(int val);
esp_err_t cam_net_isp_set_saturation(int val);

// RGB565 原始帧获取
esp_err_t cam_net_get_rgb565(uint8_t **buf, uint32_t *size);
esp_err_t cam_net_release_rgb565(void);

#endif