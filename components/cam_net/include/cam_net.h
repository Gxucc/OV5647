#ifndef CAM_NET_H
#define CAM_NET_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {

#endif

// 初始化/反初始化
esp_err_t cam_net_init(void);

// JPEG 编码
esp_err_t cam_net_get_jpeg(uint8_t **buf, uint32_t *size);
esp_err_t cam_net_encode_jpeg(uint8_t *rgb565_buf, uint32_t rgb_size,
                               uint8_t **jpeg_buf, uint32_t *jpeg_size);

// RGB565 原始帧获取
esp_err_t cam_net_get_rgb565(uint8_t **buf, uint32_t *size);
esp_err_t cam_net_release_rgb565(void);

#ifdef __cplusplus
}

#endif

#endif