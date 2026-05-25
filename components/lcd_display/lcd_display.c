#include "lcd_display.h"
#include <string.h>
#include <stdbool.h>
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

#define LCD_WIDTH           1024
#define LCD_HEIGHT          600
#define LCD_RST_GPIO        27
#define LCD_BL_GPIO         26
#define LCD_BPP             16  // RGB565

static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static uint8_t *s_lcd_buffer = NULL;

esp_err_t lcd_display_init(void)
{
    // 1. LDO 供电 MIPI PHY
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");

    // 2. MIPI-DSI bus (2 lane)
    esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus));
    ESP_LOGI(TAG, "MIPI DSI bus initialized");

    // 3. DBI IO
    esp_lcd_panel_io_handle_t mipi_dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &mipi_dbi_io));
    ESP_LOGI(TAG, "Panel IO installed");

    // 4. DPI 配置 RGB565
    const esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG_CF(LCD_COLOR_FMT_RGB565);

    // 5. vendor_config
    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };

    // 6. 面板配置
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

    // 在 lcd_display_init() 末尾，panel init 之后
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(s_lcd_panel));
    ESP_LOGI(TAG, "2D-DMA enabled for LCD");

    // 7. 背光 GPIO
    gpio_config_t bl_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LCD_BL_GPIO),
    };
    gpio_config(&bl_cfg);
    gpio_set_level(LCD_BL_GPIO, 1);
    ESP_LOGI(TAG, "Backlight on");

    // 8. 帧缓冲 PSRAM（64 字节对齐）
    s_lcd_buffer = heap_caps_aligned_alloc(64, 
                                           LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8),
                                           MALLOC_CAP_SPIRAM);
    if (!s_lcd_buffer) {
        ESP_LOGE(TAG, "Failed to alloc aligned LCD buffer");
        return ESP_FAIL;
    }

    lcd_display_clear();
    return ESP_OK;
}

void lcd_display_clear(void)
{
    if (s_lcd_buffer) {
        memset(s_lcd_buffer, 0, LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8));
        lcd_display_flush();
    }
}

// 居中显示，支持任意分辨率
// lcd_display.c
void lcd_display_camera(const uint8_t *rgb565_buf, uint32_t cam_width, uint32_t cam_height)
{
    if (!s_lcd_buffer || !rgb565_buf) return;

    // 800x640 在 1024x600 上：
    // 水平居中：左右黑边 112px
    // 垂直裁剪：640 > 600，上下各裁 20px
    int src_y_start = (cam_height - LCD_HEIGHT) / 2;   // (640 - 600) / 2 = 20
    int dst_x_offset = (LCD_WIDTH - cam_width) / 2;     // (1024 - 800) / 2 = 112
    
    // 安全边界检查
    if (cam_width > LCD_WIDTH || cam_height < LCD_HEIGHT) {
        ESP_LOGW(TAG, "Invalid cam size %dx%d for LCD %dx%d", 
                 cam_width, cam_height, LCD_WIDTH, LCD_HEIGHT);
        return;
    }
    
    for (int row = 0; row < LCD_HEIGHT; row++) {
        // 从摄像头缓冲的第 (src_y_start + row) 行读取
        const uint16_t *src = (const uint16_t*)(rgb565_buf + 
                                                (src_y_start + row) * cam_width * 2);
        // 写入 LCD 缓冲的第 row 行，水平偏移 dst_x_offset
        uint16_t *dst = (uint16_t*)(s_lcd_buffer + 
                                    row * LCD_WIDTH * 2 + 
                                    dst_x_offset * 2);
        memcpy(dst, src, cam_width * 2);  // 拷贝 800*2 = 1600 bytes
    }

    lcd_display_flush();
}

void lcd_display_flush(void)
{
    if (s_lcd_buffer && s_lcd_panel) {
        esp_cache_msync((void *)s_lcd_buffer, LCD_WIDTH * LCD_HEIGHT * (LCD_BPP / 8), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_lcd_buffer);
    }
}

uint8_t *lcd_display_get_buffer(void)
{
    return s_lcd_buffer;
}