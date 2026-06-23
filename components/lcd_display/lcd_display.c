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

static const char *TAG = "lcd_display";

#define LCD_RST_GPIO        27
#define LCD_BL_GPIO         26
#define LCD_BPP             16

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static void *s_fb[2] = {NULL, NULL};
static int s_draw_idx = 0;

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

    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &mipi_dbi_io));
    ESP_LOGI(TAG, "Panel IO installed");

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 30,
        .pixel_format = LCD_COLOR_FMT_RGB565,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
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
            .use_dma2d = 1,
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

    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(mipi_dbi_io, &panel_config, &s_lcd_panel));
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

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_lcd_panel, 2, &s_fb[0], &s_fb[1]));
    ESP_LOGI(TAG, "Double buffer acquired from driver");

    s_draw_idx = 0;

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
    s_fb[0] = NULL;
    s_fb[1] = NULL;
}

void lcd_display_copy_camera(const uint8_t *rgb565_buf, uint32_t cam_width, uint32_t cam_height)
{
    if (!s_fb[s_draw_idx] || !rgb565_buf) return;

    uint32_t copy_w = (cam_width < LCD_WIDTH) ? cam_width : LCD_WIDTH;
    uint32_t copy_h = (cam_height < LCD_HEIGHT) ? cam_height : LCD_HEIGHT;
    uint32_t copy_bytes = copy_w * copy_h * 2;

    memcpy(s_fb[s_draw_idx], rgb565_buf, copy_bytes);
}

uint8_t *lcd_display_get_buffer(void)
{
    return (uint8_t *)s_fb[s_draw_idx];
}

void lcd_display_clear(void)
{
    if (s_fb[s_draw_idx]) {
        memset(s_fb[s_draw_idx], 0, LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8));
    }
}

void lcd_display_flush(void)
{
    if (!s_fb[s_draw_idx] || !s_lcd_panel) return;

    int submit_idx = s_draw_idx;

    esp_cache_msync((void *)s_fb[submit_idx],
                    LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_fb[submit_idx]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        return;
    }

    s_draw_idx = 1 - submit_idx;
}