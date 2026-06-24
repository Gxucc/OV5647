#include "audio_recorder.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

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

static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static sdmmc_card_t *s_sd_card = NULL;
static bool s_is_recording = false;
static TaskHandle_t s_rec_task = NULL;

// 录音参数
static struct {
    int sample_rate;
    int duration_ms;
    char path[64];
} s_rec_cfg;

// 初始化 SD 卡
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

// 初始化 I2S（单声道左声道）
static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_GPIO,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = GPIO_NUM_9,
            .din = GPIO_NUM_11,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg));

    ESP_LOGI(TAG, "I2S initialized (mono left)");
    return ESP_OK;
}

// 初始化 ES8311
static esp_err_t init_es8311(i2c_master_bus_handle_t i2c_bus)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) return ESP_FAIL;

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) return ESP_FAIL;

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) return ESP_FAIL;

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = PA_CTRL_GPIO,
        .pa_reverted = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = I2S_MCLK_MULTIPLE_384,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_codec_dev) return ESP_FAIL;

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
    ESP_LOGI(TAG, "ES8311 initialized");
    return ESP_OK;
}

// 录音任务
static void recorder_task(void *arg)
{
    int sample_rate = s_rec_cfg.sample_rate;
    int duration_ms = s_rec_cfg.duration_ms;
    const char *path = s_rec_cfg.path;

    size_t sample_count = (sample_rate * duration_ms) / 1000;
    size_t buffer_size = sample_count * sizeof(int16_t);

    int16_t *buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        s_is_recording = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Recording %d ms to %s", duration_ms, path);

    // 读取数据
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(s_i2s_rx_chan, buffer, buffer_size, &bytes_read, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "Recorded %d bytes", bytes_read);

    // 保存 WAV
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        heap_caps_free(buffer);
        s_is_recording = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t data_size = bytes_read;
    uint32_t file_size = data_size + 36;

    uint8_t wav_header[44] = {
        'R', 'I', 'F', 'F',
        (file_size & 0xFF), ((file_size >> 8) & 0xFF), ((file_size >> 16) & 0xFF), ((file_size >> 24) & 0xFF),
        'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0,  // PCM
        1, 0,  // Mono
        (sample_rate & 0xFF), ((sample_rate >> 8) & 0xFF), ((sample_rate >> 16) & 0xFF), ((sample_rate >> 24) & 0xFF),
        ((sample_rate * 2) & 0xFF), (((sample_rate * 2) >> 8) & 0xFF), (((sample_rate * 2) >> 16) & 0xFF), (((sample_rate * 2) >> 24) & 0xFF),
        2, 0, 16, 0,
        'd', 'a', 't', 'a',
        (data_size & 0xFF), ((data_size >> 8) & 0xFF), ((data_size >> 16) & 0xFF), ((data_size >> 24) & 0xFF),
    };

    fwrite(wav_header, 1, 44, f);
    fwrite(buffer, 1, bytes_read, f);
    fclose(f);

    ESP_LOGI(TAG, "WAV saved: %s (%d bytes)", path, file_size);

    heap_caps_free(buffer);
    s_is_recording = false;
    s_rec_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_recorder_system_init(void *i2c_bus)
{
    ESP_LOGI(TAG, "Audio recorder system init");

    ESP_ERROR_CHECK(init_sd_card());
    ESP_ERROR_CHECK(init_i2s());
    ESP_ERROR_CHECK(init_es8311((i2c_master_bus_handle_t)i2c_bus));

    return ESP_OK;
}

bool audio_recorder_is_running(void)
{
    return s_is_recording;
}

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

    // 创建录音任务
    xTaskCreate(recorder_task, "recorder", 4096, NULL, 5, &s_rec_task);

    return ESP_OK;
}

esp_err_t audio_recorder_stop(void)
{
    if (!s_is_recording || !s_rec_task) {
        return ESP_ERR_INVALID_STATE;
    }

    // 删除任务（强制停止）
    vTaskDelete(s_rec_task);
    s_rec_task = NULL;
    s_is_recording = false;

    ESP_LOGI(TAG, "Recording stopped");
    return ESP_OK;
}

void audio_recorder_system_deinit(void)
{
    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }

    if (s_i2s_tx_chan) {
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
    }
    if (s_i2s_rx_chan) {
        i2s_del_channel(s_i2s_rx_chan);
        s_i2s_rx_chan = NULL;
    }

    if (s_sd_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_sd_card);
        s_sd_card = NULL;
    }

    ESP_LOGI(TAG, "Audio recorder deinit");
}