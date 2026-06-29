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
#include <unistd.h>

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

#define CAPTURE_SAMPLE_RATE     16000
#define CAPTURE_CHUNK_MS        100
#define CAPTURE_CHUNK_SAMPLES   (CAPTURE_SAMPLE_RATE * CAPTURE_CHUNK_MS / 1000)
#define CAPTURE_CHUNK_BYTES     (CAPTURE_CHUNK_SAMPLES * sizeof(int16_t))

#define RINGBUF_SIZE_SAMPLES    (CAPTURE_SAMPLE_RATE * 2)
#define RINGBUF_SIZE_BYTES      (RINGBUF_SIZE_SAMPLES * sizeof(int16_t))

static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static sdmmc_card_t *s_sd_card = NULL;

static bool s_is_recording = false;
static TaskHandle_t s_rec_task = NULL;

static TaskHandle_t s_capture_task = NULL;
static bool s_capture_running = false;

static int16_t *s_ringbuf = NULL;
static volatile int s_ringbuf_write_idx = 0;
static SemaphoreHandle_t s_ringbuf_mutex = NULL;

static struct {
    int sample_rate;
    int duration_ms;
    char path[64];
} s_rec_cfg;

// ==================== 环形缓冲区 ====================

static void ringbuf_init(void)
{
    s_ringbuf = (int16_t *)heap_caps_malloc(RINGBUF_SIZE_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_ringbuf) {
        s_ringbuf = (int16_t *)heap_caps_malloc(RINGBUF_SIZE_BYTES, MALLOC_CAP_INTERNAL);
    }
    memset((void *)s_ringbuf, 0, RINGBUF_SIZE_BYTES);
    s_ringbuf_write_idx = 0;
    s_ringbuf_mutex = xSemaphoreCreateMutex();
}

static void ringbuf_deinit(void)
{
    if (s_ringbuf) {
        heap_caps_free(s_ringbuf);
        s_ringbuf = NULL;
    }
    if (s_ringbuf_mutex) {
        vSemaphoreDelete(s_ringbuf_mutex);
        s_ringbuf_mutex = NULL;
    }
}

static void ringbuf_write_chunk(const int16_t *data, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        s_ringbuf[s_ringbuf_write_idx] = data[i];
        s_ringbuf_write_idx = (s_ringbuf_write_idx + 1) % RINGBUF_SIZE_SAMPLES;
    }
}

static esp_err_t ringbuf_read_recent(int16_t *out, size_t samples)
{
    if (samples > RINGBUF_SIZE_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ringbuf_mutex, portMAX_DELAY);

    int start = (s_ringbuf_write_idx - (int)samples + RINGBUF_SIZE_SAMPLES) % RINGBUF_SIZE_SAMPLES;
    for (size_t i = 0; i < samples; i++) {
        out[i] = s_ringbuf[(start + i) % RINGBUF_SIZE_SAMPLES];
    }

    xSemaphoreGive(s_ringbuf_mutex);
    return ESP_OK;
}

// ==================== SD 卡 ====================

static esp_err_t init_sd_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 10000;
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

// ==================== I2S ====================

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 128;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CAPTURE_SAMPLE_RATE),
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
    ESP_LOGI(TAG, "I2S RX initialized");
    return ESP_OK;
}

// ==================== ES8311 ====================

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

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = NULL,
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
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
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
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
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
        .sample_rate = CAPTURE_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_codec_dev, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open codec");
        return ESP_FAIL;
    }

    esp_codec_dev_set_in_gain(s_codec_dev, 42);
    ESP_LOGI(TAG, "ES8311 initialized (ADC only)");
    return ESP_OK;
}

// ==================== 采集任务 ====================

static void capture_task(void *arg)
{
    int16_t *chunk_buf = (int16_t *)heap_caps_malloc(CAPTURE_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (!chunk_buf) {
        chunk_buf = (int16_t *)heap_caps_malloc(CAPTURE_CHUNK_BYTES, MALLOC_CAP_INTERNAL);
    }
    if (!chunk_buf) {
        ESP_LOGE(TAG, "Capture chunk buffer alloc failed");
        s_capture_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Capture task started");

    while (s_capture_running) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_i2s_rx_chan, chunk_buf, CAPTURE_CHUNK_BYTES, &bytes_read, pdMS_TO_TICKS(CAPTURE_CHUNK_MS + 50));

        if (ret == ESP_OK && bytes_read > 0) {
            size_t samples = bytes_read / sizeof(int16_t);
            ringbuf_write_chunk(chunk_buf, samples);
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    ESP_LOGI(TAG, "Capture task stopped");
    heap_caps_free(chunk_buf);
    s_capture_task = NULL;
    vTaskDelete(NULL);
}

// ==================== 录音任务（直接读 I2S） ====================

static void recorder_task(void *arg)
{
    int sample_rate = s_rec_cfg.sample_rate;
    int duration_ms = s_rec_cfg.duration_ms;
    const char *path = s_rec_cfg.path;

    ESP_LOGI(TAG, "recorder_task started: sr=%d, dur=%d, path=%s", sample_rate, duration_ms, path);

    size_t chunk_bytes = CAPTURE_CHUNK_SAMPLES * sizeof(int16_t);
    int16_t *chunk_buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_SPIRAM);
    if (!chunk_buf) {
        chunk_buf = heap_caps_malloc(chunk_bytes, MALLOC_CAP_INTERNAL);
    }
    if (!chunk_buf) {
        ESP_LOGE(TAG, "Failed to allocate chunk buffer");
        s_is_recording = false;
        s_rec_task = NULL;
        vTaskDelete(NULL);
        return;
    }

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

    uint8_t placeholder[44] = {0};
    fwrite(placeholder, 1, 44, f);

    size_t total_bytes = 0;
    int64_t start_time = esp_timer_get_time();

    ESP_LOGI(TAG, "Recording started, max duration %d ms", duration_ms);

    while (s_is_recording) {
        int64_t elapsed_ms = (esp_timer_get_time() - start_time) / 1000;
        if (elapsed_ms >= duration_ms) {
            ESP_LOGI(TAG, "Reached max duration %d ms", duration_ms);
            break;
        }

        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_i2s_rx_chan, chunk_buf, chunk_bytes, &bytes_read, pdMS_TO_TICKS(CAPTURE_CHUNK_MS + 50));

        if (ret == ESP_OK && bytes_read > 0) {
            size_t written = fwrite(chunk_buf, 1, bytes_read, f);
            if (written != bytes_read) {
                ESP_LOGE(TAG, "fwrite failed: %d/%d", written, bytes_read);
                break;
            }
            total_bytes += bytes_read;
        } else if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(ret));
        }

        if (!s_is_recording) {
            ESP_LOGI(TAG, "Recording stopped by user");
            break;
        }
    }

    ESP_LOGI(TAG, "Recording finished, total %d bytes", total_bytes);

    fflush(f);
    fsync(fileno(f));

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

    heap_caps_free(chunk_buf);
    s_is_recording = false;
    s_rec_task = NULL;

    // 录音结束，恢复采集任务
    audio_capture_start();

    vTaskDelete(NULL);
}

// ==================== 公共 API ====================

esp_err_t audio_recorder_system_init(void *i2c_bus)
{
    ESP_LOGI(TAG, "Audio recorder system init");
    ESP_ERROR_CHECK(init_sd_card());
    ESP_ERROR_CHECK(init_i2s());
    ESP_ERROR_CHECK(init_es8311((i2c_master_bus_handle_t)i2c_bus));
    ringbuf_init();
    return ESP_OK;
}

bool audio_recorder_is_running(void) { return s_is_recording; }

esp_err_t audio_recorder_start(const audio_recorder_cfg_t *cfg)
{
    if (s_is_recording) {
        ESP_LOGW(TAG, "Already recording");
        return ESP_ERR_INVALID_STATE;
    }

    // 等待上次任务退出
    int wait = 0;
    while (s_rec_task && wait < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait++;
    }
    if (s_rec_task) {
        ESP_LOGW(TAG, "Previous recorder task still alive, forcing NULL");
        s_rec_task = NULL;
    }

    // 暂停采集任务
    audio_capture_stop();

    s_rec_cfg.sample_rate = cfg && cfg->sample_rate ? cfg->sample_rate : CAPTURE_SAMPLE_RATE;
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

// ==================== 实时采样读取（采集任务运行时） ====================

esp_err_t audio_recorder_read_samples(int16_t *buffer, size_t samples, int timeout_ms)
{
    if (!buffer || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;

    while (1) {
        esp_err_t ret = ringbuf_read_recent(buffer, samples);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(10));

        if ((esp_timer_get_time() - start) > timeout_us) {
            ESP_LOGW(TAG, "Read timeout");
            return ESP_ERR_TIMEOUT;
        }
    }
}

// ==================== 采集控制 ====================

esp_err_t audio_capture_start(void)
{
    if (s_capture_running) {
        ESP_LOGW(TAG, "Capture already running");
        return ESP_OK;
    }

    s_capture_running = true;
    BaseType_t ret = xTaskCreate(capture_task, "capture", 4096, NULL, 5, &s_capture_task);
    if (ret != pdPASS) {
        s_capture_running = false;
        ESP_LOGE(TAG, "Failed to create capture task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio capture started");
    return ESP_OK;
}

void audio_capture_stop(void)
{
    if (!s_capture_running) {
        return;
    }

    s_capture_running = false;
    int wait = 0;
    while (s_capture_task && wait < 50) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait++;
    }
    ESP_LOGI(TAG, "Audio capture stopped");
}

bool audio_capture_is_running(void)
{
    return s_capture_running;
}

// ==================== 反初始化 ====================

void audio_recorder_system_deinit(void)
{
    audio_capture_stop();

    if (s_codec_dev) {
        esp_codec_dev_close(s_codec_dev);
        esp_codec_dev_delete(s_codec_dev);
        s_codec_dev = NULL;
    }
    if (s_i2s_rx_chan) {
        i2s_del_channel(s_i2s_rx_chan);
        s_i2s_rx_chan = NULL;
    }
    if (s_sd_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_sd_card);
        s_sd_card = NULL;
    }
    ringbuf_deinit();
    ESP_LOGI(TAG, "Audio recorder deinit");
}

// ==================== 保存 PCM ====================

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