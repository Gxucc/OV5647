#include "lcd_display.h"
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/gpio.h"

#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

// 引入 adapter 头文件用于获取缓冲区数量
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"

static const char *TAG = "lcd_display";

#define LCD_RST_GPIO        27
#define LCD_BL_GPIO         26
#define LCD_BPP             16

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_mipi_dbi_io = NULL;

// 动态分配帧缓冲指针数组，最大支持 3 个缓冲区
#define MAX_FB_COUNT 3
static void *s_fb[MAX_FB_COUNT] = {NULL};
static uint8_t s_fb_count = 0;

esp_err_t lcd_display_init(void)
{
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");

    esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus));
    ESP_LOGI(TAG, "MIPI DSI bus initialized");

    s_mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_mipi_dbi_io));
    ESP_LOGI(TAG, "Panel IO installed");

    // ==== 关键修改：使用 adapter 计算所需的帧缓冲数量 ====
    // 注意：旋转角度必须与 lvgl_ui.c 中注册显示时使用的角度一致
    s_fb_count = esp_lv_adapter_get_required_frame_buffer_count(
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB,  // 防撕裂模式
        0                                            // 旋转角度 (0 度)
    );
    
    // 确保缓冲区数量不超过预定义的最大值
    if (s_fb_count > MAX_FB_COUNT) {
        ESP_LOGW(TAG, "Required frame buffers (%d) exceed MAX_FB_COUNT (%d), clamping", s_fb_count, MAX_FB_COUNT);
        s_fb_count = MAX_FB_COUNT;
    }
    ESP_LOGI(TAG, "Required frame buffers: %d", s_fb_count);

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 30,
        .pixel_format = LCD_COLOR_FMT_RGB565,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = s_fb_count,  // 使用动态计算的值
        .video_timing = {
            .h_size = LCD_WIDTH,
            .v_size = LCD_HEIGHT,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .hsync_pulse_width = 10,
            .vsync_back_porch = 10,
            .vsync_front_porch = 10,
            .vsync_pulse_width = 2,
        },
        .flags = {
            .use_dma2d = 0,  //DMA2D
            .disable_lp = 0,
        },
    };

    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(s_mipi_dbi_io, &panel_config, &s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd_panel));
    ESP_LOGI(TAG, "EK79007 panel initialized");

    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_BL_GPIO),
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_BL_GPIO, 1);
    ESP_LOGI(TAG, "Backlight on");

    // 获取所有帧缓冲
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_lcd_panel, s_fb_count, s_fb));
    
    // 打印每个缓冲区地址用于调试
    for (int i = 0; i < s_fb_count; i++) {
        ESP_LOGI(TAG, "Frame buffer %d: %p", i, s_fb[i]);
    }

    return ESP_OK;
}

void lcd_display_deinit(void)
{
    if (s_lcd_panel) {
        esp_lcd_panel_del(s_lcd_panel);
        s_lcd_panel = NULL;
    }

    if (s_dsi_bus) {
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
    }
    
    // 清空帧缓冲指针
    for (int i = 0; i < MAX_FB_COUNT; i++) {
        s_fb[i] = NULL;
    }
    s_fb_count = 0;
}

void lcd_display_copy_camera(const uint8_t *rgb565_buf, uint32_t cam_width, uint32_t cam_height)
{
    // 默认使用第一个缓冲区
    if (!s_fb[0] || !rgb565_buf) return;

    uint32_t copy_w = (cam_width < LCD_WIDTH) ? cam_width : LCD_WIDTH;
    uint32_t copy_h = (cam_height < LCD_HEIGHT) ? cam_height : LCD_HEIGHT;
    uint32_t copy_bytes = copy_w * copy_h * 2;

    memcpy(s_fb[0], rgb565_buf, copy_bytes);
}

uint8_t *lcd_display_get_buffer(void)
{
    // 默认返回第一个缓冲区
    return (uint8_t *)s_fb[0];
}

// 新增：获取指定索引的缓冲区
uint8_t *lcd_display_get_buffer_idx(int idx)
{
    if (idx < 0 || idx >= s_fb_count) {
        return NULL;
    }
    return (uint8_t *)s_fb[idx];
}

// 新增：获取缓冲区数量
uint8_t lcd_display_get_buffer_count(void)
{
    return s_fb_count;
}

void lcd_display_clear(void)
{
    if (s_fb[0]) {
        memset(s_fb[0], 0, LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8));
    }
}

void lcd_display_flush(void)
{
    if (!s_fb[0] || !s_lcd_panel) return;

    esp_cache_msync((void *)s_fb[0],
                    LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_fb[0]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        return;
    }
}

esp_lcd_panel_handle_t lcd_display_get_panel(void)
{
    return s_lcd_panel;
}

esp_lcd_panel_io_handle_t lcd_display_get_io(void)
{
    return s_mipi_dbi_io;
}