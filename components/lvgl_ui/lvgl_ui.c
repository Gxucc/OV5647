#include "lvgl_ui.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "lcd_display.h"
#include "touch_gt911.h"

static const char *TAG = "lvgl_ui";

static lv_display_t *s_disp = NULL;
static ui_state_t s_state = UI_STATE_HOME;

// 主界面对象
static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_btn_fall_debug = NULL;

// 跌倒检测调试界面对象
static lv_obj_t *s_fall_debug_screen = NULL;

// 标志位
static volatile bool s_should_start_fall_debug = false;
static volatile bool s_should_return_home = false;

// 触摸读取回调函数
static void lvgl_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    int x, y;
    bool pressed;
    
    if (touch_gt911_read(&x, &y, &pressed) == ESP_OK && pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD("touch", "pressed: x=%d, y=%d", x, y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// 按钮事件回调
static void btn_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (btn == s_btn_fall_debug) {
        ESP_LOGI(TAG, "Button clicked: enter fall debug");
        s_should_start_fall_debug = true;
    }
}

// 创建主界面
static void create_home_screen(void)
{
    s_home_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_size(s_home_screen, LCD_WIDTH, LCD_HEIGHT);

    lv_obj_t *label_title = lv_label_create(s_home_screen);
    lv_label_set_text(label_title, "Family Care System");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, 0);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 60);

    s_btn_fall_debug = lv_btn_create(s_home_screen);
    lv_obj_set_size(s_btn_fall_debug, 320, 80);
    lv_obj_align(s_btn_fall_debug, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_btn_fall_debug, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(s_btn_fall_debug, lv_color_hex(0x0f3460), LV_STATE_PRESSED);

    lv_obj_t *label_btn = lv_label_create(s_btn_fall_debug);
    lv_label_set_text(label_btn, "Fall Detection Debug");
    lv_obj_set_style_text_color(label_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_btn, &lv_font_montserrat_14, 0);
    lv_obj_center(label_btn);

    lv_obj_add_event_cb(s_btn_fall_debug, btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_scr_load(s_home_screen);
}

// 创建跌倒检测调试界面
static void create_fall_debug_screen(void)
{
    s_fall_debug_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_fall_debug_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_size(s_fall_debug_screen, LCD_WIDTH, LCD_HEIGHT);

    // 提示文字（用英文避免字体问题，后续替换为中文）
    lv_obj_t *label = lv_label_create(s_fall_debug_screen);
    lv_label_set_text(label, "Swipe right to return");
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

    // 添加右滑手势检测区域
    lv_obj_t *gesture_area = lv_obj_create(s_fall_debug_screen);
    lv_obj_set_size(gesture_area, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(gesture_area, LV_OPA_0, 0);  // 透明
    lv_obj_set_style_border_width(gesture_area, 0, 0);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_SCROLLABLE);

    // 设置滚动方向为水平，用于检测右滑
    lv_obj_set_scroll_dir(gesture_area, LV_DIR_HOR);
}

// 右滑返回手势检测
static void fall_debug_event_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    if (dir == LV_DIR_RIGHT) {
        ESP_LOGI(TAG, "Swipe right detected: return to home");
        s_should_return_home = true;
    }
}

esp_err_t lvgl_ui_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL UI");

    // 获取面板和 IO
    esp_lcd_panel_handle_t panel = lcd_display_get_panel();
    esp_lcd_panel_io_handle_t io = lcd_display_get_io();
    if (!panel || !io) {
        ESP_LOGE(TAG, "Failed to get panel or io handle");
        return ESP_FAIL;
    }

    // 初始化适配器
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));

    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
        ESP_LV_ADAPTER_ROTATE_0
    );
    ESP_LOGI(TAG, "Required frame buffers for DOUBLE_FULL mode: %d", num_fbs);

    // 注册 MIPI DSI 显示
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        panel,
        io,
        LCD_WIDTH,
        LCD_HEIGHT,
        ESP_LV_ADAPTER_ROTATE_0
    );
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "Failed to register display");
        return ESP_FAIL;
    }

    // 启动适配器
    ESP_ERROR_CHECK(esp_lv_adapter_start());

    // 初始化触摸
    ESP_ERROR_CHECK(touch_gt911_init());

    // 注册 LVGL 输入设备
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read);

    // 创建界面
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        create_home_screen();
        create_fall_debug_screen();

        // 添加手势事件到调试界面
        lv_obj_add_event_cb(s_fall_debug_screen, fall_debug_event_cb, LV_EVENT_GESTURE, NULL);

        esp_lv_adapter_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL UI initialized, state: HOME");
    return ESP_OK;
}

void lvgl_ui_deinit(void)
{
    esp_lv_adapter_deinit();
}

ui_state_t lvgl_ui_get_state(void)
{
    return s_state;
}

void lvgl_ui_set_state(ui_state_t state)
{
    s_state = state;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (state == UI_STATE_HOME) {
            lv_scr_load(s_home_screen);
            s_should_return_home = false;
        } else if (state == UI_STATE_FALL_DEBUG) {
            lv_scr_load(s_fall_debug_screen);
            s_should_start_fall_debug = false;
        }
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "State changed to: %s", state == UI_STATE_HOME ? "HOME" : "FALL_DEBUG");
}

void lvgl_ui_task_handler(void)
{
    // 检查右滑返回
    if (s_state == UI_STATE_FALL_DEBUG && s_should_return_home) {
        lvgl_ui_set_state(UI_STATE_HOME);
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}

bool lvgl_ui_should_start_fall_debug(void)
{
    bool ret = s_should_start_fall_debug;
    s_should_start_fall_debug = false;
    return ret;
}

bool lvgl_ui_should_return_home(void)
{
    bool ret = s_should_return_home;
    s_should_return_home = false;
    return ret;
}

void lvgl_ui_set_return_home(void)
{
    s_should_return_home = true;
}