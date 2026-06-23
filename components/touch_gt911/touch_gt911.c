#include "touch_gt911.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"

static const char *TAG = "touch_gt911";

#define I2C_CLK_SPEED_HZ    400000

static esp_lcd_touch_handle_t s_tp = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;

esp_err_t touch_gt911_init(void)
{
    ESP_LOGI(TAG, "Initializing GT911 touch");

    // 获取已存在的 I2C 总线（由 cam_net.c 创建）
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(I2C_NUM_0, &s_i2c_bus));
    ESP_LOGI(TAG, "Reusing I2C bus from cam_net");

    // 创建触摸 I2C IO
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = {
            .disable_control_phase = 1,
        },
        .scl_speed_hz = I2C_CLK_SPEED_HZ,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io));

    // 创建 GT911 触摸驱动
    esp_lcd_touch_config_t touch_cfg = {
        .x_max = 1024,
        .y_max = 600,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &touch_cfg, &s_tp));

    ESP_LOGI(TAG, "GT911 touch initialized successfully");
    return ESP_OK;
}

esp_err_t touch_gt911_read(int *x, int *y, bool *pressed)
{
    uint16_t touch_x, touch_y;
    uint16_t strength;
    uint8_t finger_num;

    esp_lcd_touch_read_data(s_tp);
    bool touched = esp_lcd_touch_get_coordinates(s_tp, &touch_x, &touch_y, &strength, &finger_num, 1);

    if (touched) {
        *x = touch_x;
        *y = touch_y;
        *pressed = true;
        ESP_LOGD(TAG, "Touch: x=%d, y=%d, strength=%d", touch_x, touch_y, strength);
    } else {
        *pressed = false;
    }

    return ESP_OK;
}