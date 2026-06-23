#include "cam_net.h"
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "driver/jpeg_encode.h"

static const char *TAG = "cam_net";

#define CAM_DEV_NAME    ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define BUFFER_COUNT    2

static int s_fd = -1;
static uint8_t *s_buffers[BUFFER_COUNT];
static uint32_t s_buffer_size = 0;

// JPEG 编码器
static jpeg_encoder_handle_t s_jpeg_enc = NULL;
static uint8_t *s_jpeg_out_buf = NULL;
static uint32_t s_jpeg_out_buf_size = 0;

#define CAM_WIDTH   1024
#define CAM_HEIGHT  600

// ISP 句柄（自动模式，无需手动配置）
static int s_fd_isp = -1;

esp_err_t cam_net_init(void)
{
    // 1. 初始化 esp_video
    const esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = I2C_NUM_0,
                .scl_pin = 8,
                .sda_pin = 7,
            },
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };

    const esp_video_init_config_t cam_config = {
        .csi = &csi_config,
    };

    ESP_ERROR_CHECK(esp_video_init(&cam_config));
    ESP_LOGI(TAG, "esp_video init done");

    // 2. 打开视频设备
    s_fd = open(CAM_DEV_NAME, O_RDWR);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", CAM_DEV_NAME);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opened %s", CAM_DEV_NAME);

    // 3. 设置格式
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_WIDTH;
    fmt.fmt.pix.height = CAM_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) < 0) {
        ESP_LOGE(TAG, "Failed to set format");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Format set: %dx%d RGB565", fmt.fmt.pix.width, fmt.fmt.pix.height);

    // 4. 申请缓冲
    struct v4l2_requestbuffers req = {0};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request buffers");
        return ESP_FAIL;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to query buffer %d", i);
            return ESP_FAIL;
        }

        s_buffers[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        s_buffer_size = buf.length;

        if (ioctl(s_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue buffer %d", i);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Buffers allocated: %d x %u bytes", BUFFER_COUNT, s_buffer_size);

    // 5. 启动流
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start stream");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Stream started");

    // 6. 打开 ISP 设备（自动 Pipeline 模式）
    s_fd_isp = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (s_fd_isp < 0) {
        ESP_LOGW(TAG, "ISP device open failed");
    } else {
        ESP_LOGI(TAG, "ISP auto mode enabled");
    }

    // 屏蔽 ISP_AWB 警告日志(自动白平衡的bug)
    esp_log_level_set("ISP_AWB", ESP_LOG_ERROR);

    // 7. JPEG 编码器
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    ESP_ERROR_CHECK(jpeg_new_encoder_engine(&engine_cfg, &s_jpeg_enc));

    uint32_t raw_size = CAM_WIDTH * CAM_HEIGHT * 2;
    s_jpeg_out_buf_size = raw_size / 5;

    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated_size = 0;

    s_jpeg_out_buf = jpeg_alloc_encoder_mem(s_jpeg_out_buf_size, &mem_cfg, &allocated_size);
    if (!s_jpeg_out_buf) {
        ESP_LOGE(TAG, "Failed to alloc JPEG output buffer");
        return ESP_FAIL;
    }
    s_jpeg_out_buf_size = allocated_size;
    ESP_LOGI(TAG, "JPEG encoder ready");

    return ESP_OK;
}

// ========== 帧获取 ==========

static int s_current_buf_index = -1;

esp_err_t cam_net_get_rgb565(uint8_t **buf, uint32_t *size)
{
    struct v4l2_buffer buf_v4l2 = {0};
    buf_v4l2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf_v4l2.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_fd, VIDIOC_DQBUF, &buf_v4l2) < 0) {
        ESP_LOGE(TAG, "DQBUF failed");
        return ESP_FAIL;
    }

    s_current_buf_index = buf_v4l2.index;

    // ISP Pipeline 自动处理画质
    *buf = s_buffers[buf_v4l2.index];
    *size = buf_v4l2.bytesused;

    return ESP_OK;
}

esp_err_t cam_net_release_rgb565(void)
{
    if (s_current_buf_index < 0 || s_current_buf_index >= BUFFER_COUNT) {
        return ESP_FAIL;
    }

    struct v4l2_buffer buf_v4l2 = {0};
    buf_v4l2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf_v4l2.memory = V4L2_MEMORY_MMAP;
    buf_v4l2.index = s_current_buf_index;

    if (ioctl(s_fd, VIDIOC_QBUF, &buf_v4l2) < 0) {
        ESP_LOGE(TAG, "QBUF failed");
        return ESP_FAIL;
    }

    s_current_buf_index = -1;
    return ESP_OK;
}

// ========== JPEG 编码 ==========

esp_err_t cam_net_get_jpeg(uint8_t **buf, uint32_t *size)
{
    struct v4l2_buffer buf_v4l2 = {0};
    buf_v4l2.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf_v4l2.memory = V4L2_MEMORY_MMAP;

    if (ioctl(s_fd, VIDIOC_DQBUF, &buf_v4l2) < 0) {
        ESP_LOGE(TAG, "DQBUF failed");
        return ESP_FAIL;
    }

    uint8_t *rgb_buf = s_buffers[buf_v4l2.index];
    uint32_t rgb_size = buf_v4l2.bytesused;

    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 90,
        .width = CAM_WIDTH,
        .height = CAM_HEIGHT,
    };

    uint32_t jpeg_size = 0;
    esp_err_t ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg,
                                          rgb_buf, rgb_size,
                                          s_jpeg_out_buf, s_jpeg_out_buf_size,
                                          &jpeg_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed");
        ioctl(s_fd, VIDIOC_QBUF, &buf_v4l2);
        return ret;
    }

    *buf = s_jpeg_out_buf;
    *size = jpeg_size;

    if (ioctl(s_fd, VIDIOC_QBUF, &buf_v4l2) < 0) {
        ESP_LOGE(TAG, "QBUF failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t cam_net_encode_jpeg(uint8_t *rgb565_buf, uint32_t rgb_size,
                               uint8_t **jpeg_buf, uint32_t *jpeg_size)
{
    if (!rgb565_buf || rgb_size == 0) return ESP_FAIL;

    jpeg_encode_cfg_t enc_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = 45,
        .width = CAM_WIDTH,
        .height = CAM_HEIGHT,
    };

    uint32_t out_size = 0;
    esp_err_t ret = jpeg_encoder_process(s_jpeg_enc, &enc_cfg,
                                          rgb565_buf, rgb_size,
                                          s_jpeg_out_buf, s_jpeg_out_buf_size,
                                          &out_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG encode failed");
        return ret;
    }

    *jpeg_buf = s_jpeg_out_buf;
    *jpeg_size = out_size;
    return ESP_OK;
}
