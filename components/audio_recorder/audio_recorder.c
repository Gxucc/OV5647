#include "audio_recorder.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "audio_recorder";

#define I2S_NUM             I2S_NUM_0
#define I2S_MCLK_GPIO       GPIO_NUM_13
#define I2S_BCLK_GPIO       GPIO_NUM_12
#define I2S_WS_GPIO         GPIO_NUM_10
#define PA_CTRL_GPIO        GPIO_NUM_53

#define SDMMC_CLK_GPIO      GPIO_NUM_43
#define SDMMC_CMD_GPIO      GPIO_NUM_44
#define SDMMC_D0_GPIO       GPIO_NUM_39
#define SDMMC_D1_GPIO       GPIO_NUM_40
#define SDMMC_D2_GPIO       GPIO_NUM_41
#define SDMMC_D3_GPIO       GPIO_NUM_42

#define MOUNT_POINT         "/sdcard"
#define REC_CHUNK_MS        100
#define REC_CHUNK_SAMPLES   (16000 * REC_CHUNK_MS / 1000)  // 1600 samples

static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static sdmmc_card_t *s_sd_card = NULL;
static bool s_is_recording = false;
static TaskHandle_t s_rec_task = NULL;

static struct {
    int sample_rate;
    int duration_ms;
    char path[64];
} s_rec_cfg;

// ==================== SD 卡 ====================

static esp_err_t init_sd_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SDMMC_CLK_GPIO;
    slot_config.cmd = SDMMC_CMD_GPIO;
    slot_config.d0 = SDMMC_D0_GPIO;
    slot_config.d1 = SDMMC_D1_GPIO;
    slot_config.d2 = SDMMC_D2_GPIO;
    slot_config.d3 = SDMMC_D3_GPIO;
    slot_config.width = 4;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    return ESP_OK;
}

// ==================== I2S (仅 RX) ====================

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    // 极致减小 DMA 配置
    chan_cfg.dma_desc_num = 2;      // 最小 DMA 描述符数量
    chan_cfg.dma_frame_num = 64;    // 最小帧数
    
    // 关键：确保 DMA 缓冲区分配在内部 RAM，避免 PSRAM
    chan_cfg.dma_frame_num = 64;
    
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = GPIO_NUM_NC,
            .din = GPIO_NUM_11,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg));
    ESP_LOGI(TAG, "I2S RX initialized (dma_desc=2, frame_num=64)");
    return ESP_OK;
}

// ==================== ES8311 初始化 ====================

static esp_err_t init_es8311(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "init_es8311 with bus=%p", i2c_bus);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };

    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "audio_codec_new_i2c_ctrl success");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = NULL,  // 没有 TX 通道
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        audio_codec_delete_ctrl_if(ctrl_if);
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) {
        audio_codec_delete_ctrl_if(ctrl_if);
        audio_codec_delete_data_if(data_if);
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,  // 仅 ADC 模式（录音）
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = PA_CTRL_GPIO,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = I2S_MCLK_MULTIPLE_256,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) {
        audio_codec_delete_ctrl_if(ctrl_if);
        audio_codec_delete_data_if(data_if);
        audio_codec_delete_gpio_if(gpio_if);
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,  // 仅输入
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_codec_dev) {
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = 0x01,
        .sample_rate = 16000,
    };
    if (esp_codec_dev_open(s_codec_dev, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec");
        return ESP_FAIL;
    }

    esp_codec_dev_set_in_gain(s_codec_dev, 42);
    ESP_LOGI(TAG, "ES8311 initialized (ADC only)");
    return ESP_OK;
}

// ==================== 流式录音任务 ====================

static void recorder_task(void *arg)
{
    int sample_rate = s_rec_cfg.sample_rate;
    int duration_ms = s_rec_cfg.duration_ms;
    const char *path = s_rec_cfg.path;

    ESP_LOGI(TAG, "recorder_task started: sr=%d, dur=%d, path=%s", sample_rate, duration_ms, path);

    // 分配 chunk 缓冲区（内部 RAM，仅 3.2KB）
    size_t chunk_bytes = REC_CHUNK_SAMPLES * sizeof(int16_t);
    int16_t *chunk_buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_INTERNAL);
    if (!chunk_buf) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        s_is_recording = false;
        s_rec_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 打开文件，预留 WAV 头位置
    ESP_LOGI(TAG, "Opening %s for writing", path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        heap_caps_free(chunk_buf);
        s_is_recording = false;
        s_rec_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 写入占位 WAV 头（44 字节）
    uint8_t placeholder[44] = {0};
    fwrite(placeholder, 1, 44, f);

    // 配置 ES8311 采样率
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 1,
        .channel_mask = 0x01,
        .sample_rate = sample_rate,
    };

    ESP_LOGI(TAG, "Opening codec for read");
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec for read");
        fclose(f);
        heap_caps_free(chunk_buf);
        s_is_recording = false;
        s_rec_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 录音循环：边录边写
    size_t total_bytes = 0;
    int64_t start_time = esp_timer_get_time();

    ESP_LOGI(TAG, "Recording started, max duration %d ms", duration_ms);

    while (s_is_recording) {
        // 检查是否超时
        int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
        if (elapsed_ms >= duration_ms) {
            ESP_LOGI(TAG, "Reached max duration %d ms", duration_ms);
            break;
        }

        // 读取音频 chunk
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_i2s_rx_chan, chunk_buf, chunk_bytes, &bytes_read, pdMS_TO_TICKS(REC_CHUNK_MS + 50));

        if (ret == ESP_OK && bytes_read > 0) {
            // 直接写入文件
            size_t written = fwrite(chunk_buf, 1, bytes_read, f);
            if (written != bytes_read) {
                ESP_LOGE(TAG, "fwrite failed: %d/%d", written, bytes_read);
                break;
            }
            total_bytes += bytes_read;
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));
        }

        // 检查是否被请求停止
        if (!s_is_recording) {
            ESP_LOGI(TAG, "Recording stopped by user");
            break;
        }
    }

    ESP_LOGI(TAG, "Recording finished, total %d bytes", total_bytes);

    // 关闭 codec
    esp_codec_dev_close(s_codec_dev);

    // 回到文件开头，写入正确的 WAV 头
    fseek(f, 0, SEEK_SET);

    uint32_t data_size = total_bytes;
    uint32_t file_size = data_size + 36;
    uint8_t wav_header[44] = {
        'R','I','F','F',
        (file_size & 0xFF), ((file_size >> 8) & 0xFF), ((file_size >> 16) & 0xFF), ((file_size >> 24) & 0xFF),
        'W','A','V','E',
        'f','m','t',' ', 16, 0, 0, 0,
        1, 0, 1, 0,
        (sample_rate & 0xFF), ((sample_rate >> 8) & 0xFF), ((sample_rate >> 16) & 0xFF), ((sample_rate >> 24) & 0xFF),
        ((sample_rate * 2) & 0xFF), (((sample_rate * 2) >> 8) & 0xFF), (((sample_rate * 2) >> 16) & 0xFF), (((sample_rate * 2) >> 24) & 0xFF),
        2, 0, 16, 0,
        'd','a','t','a',
        (data_size & 0xFF), ((data_size >> 8) & 0xFF), ((data_size >> 16) & 0xFF), ((data_size >> 24) & 0xFF),
    };

    fwrite(wav_header, 1, 44, f);
    fclose(f);

    ESP_LOGI(TAG, "WAV saved: %s (%d bytes)", path, file_size);

    // 清理
    heap_caps_free(chunk_buf);
    s_is_recording = false;
    s_rec_task = NULL;
    vTaskDelete(NULL);
}

// ==================== 公共 API ====================

esp_err_t audio_recorder_system_init(void *i2c_bus)
{
    ESP_LOGI(TAG, "Audio recorder system init");
    // ESP_ERROR_CHECK(init_sd_card());  // 屏蔽 SD 卡
    ESP_ERROR_CHECK(init_i2s());
    ESP_ERROR_CHECK(init_es8311((i2c_master_bus_handle_t)i2c_bus));
    return ESP_OK;
}

bool audio_recorder_is_running(void) { return s_is_recording; }

esp_err_t audio_recorder_start(const audio_recorder_cfg_t *cfg)
{
    if (s_is_recording) {
        ESP_LOGW(TAG, "Already recording");
        return ESP_ERR_INVALID_STATE;
    }
    s_rec_cfg.sample_rate = cfg && cfg->sample_rate ? cfg->sample_rate : 16000;
    s_rec_cfg.duration_ms = cfg && cfg->duration_ms ? cfg->duration_ms : 10000;
    strncpy(s_rec_cfg.path, cfg && cfg->save_path ? cfg->save_path : "/sdcard/rec_default.wav", sizeof(s_rec_cfg.path) - 1);
    s_rec_cfg.path[sizeof(s_rec_cfg.path) - 1] = '\0';
    s_is_recording = true;
    xTaskCreate(recorder_task, "recorder", 4096, NULL, 5, &s_rec_task);
    return ESP_OK;
}

esp_err_t audio_recorder_stop(void)
{
    if (!s_is_recording) return ESP_ERR_INVALID_STATE;
    s_is_recording = false;
    ESP_LOGI(TAG, "Recording stop requested");
    return ESP_OK;
}

// ==================== 内存采集 API ====================

esp_err_t audio_recorder_read_samples(int16_t *buffer, size_t samples, int timeout_ms)
{
    if (!s_i2s_rx_chan || !buffer || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes_to_read = samples * sizeof(int16_t);
    size_t bytes_read = 0;
    size_t offset = 0;

    // 分块读取，直到凑够 samples
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (offset < bytes_to_read) {
        size_t chunk = 0;
        esp_err_t ret = i2s_channel_read(
            s_i2s_rx_chan,
            (uint8_t *)buffer + offset,
            bytes_to_read - offset,
            &chunk,
            pdMS_TO_TICKS(100)
        );

        if (ret == ESP_OK && chunk > 0) {
            offset += chunk;
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read error: %s", esp_err_to_name(ret));
        }

        // 检查超时
        if ((esp_timer_get_time() - start) > timeout_us) {
            ESP_LOGW(TAG, "Read timeout: got %d/%d samples", (int)(offset / sizeof(int16_t)), (int)samples);
            return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

void audio_recorder_system_deinit(void)
{
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    // 只删除 RX 通道
    if (s_i2s_rx_chan) { i2s_del_channel(s_i2s_rx_chan); s_i2s_rx_chan = NULL; }
    if (s_sd_card) { esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_sd_card); s_sd_card = NULL; }
    ESP_LOGI(TAG, "Audio recorder deinit");
}

esp_err_t audio_recorder_save_pcm(const char *path, const int16_t *buffer, size_t samples)
{
    if (!path || !buffer || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer, sizeof(int16_t), samples, f);
    fclose(f);

    if (written != samples) {
        ESP_LOGE(TAG, "PCM write failed: %d/%d", (int)written, (int)samples);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PCM saved: %s (%d samples, %d bytes)", path, (int)samples, (int)(samples * sizeof(int16_t)));
    return ESP_OK;
}