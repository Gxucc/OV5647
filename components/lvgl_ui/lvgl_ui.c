#include "lvgl_ui.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lv_adapter.h"
#include "lcd_display.h"
#include "touch_gt911.h"
#include "audio_recorder.h"

static const char *TAG = "lvgl_ui";

static lv_display_t *s_disp = NULL;
static ui_state_t s_state = UI_STATE_HOME;

// 主界面对象
static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_btn_fall_debug = NULL;
static lv_obj_t *s_btn_record = NULL;
static lv_obj_t *s_btn_babysound = NULL;

// 跌倒检测调试界面对象
static lv_obj_t *s_fall_debug_screen = NULL;

// 录音界面对象
static lv_obj_t *s_record_screen = NULL;
static lv_obj_t *s_record_btn = NULL;
static lv_obj_t *s_record_label_status = NULL;
static lv_obj_t *s_record_label_btn = NULL;
static lv_obj_t *s_record_label_send_status = NULL;

// 婴儿声音检测界面对象
static lv_obj_t *s_babysound_screen = NULL;
static lv_obj_t *s_babysound_switch = NULL;
static lv_obj_t *s_babysound_label_status = NULL;

// 标志位
static volatile bool s_should_start_fall_debug = false;
static volatile bool s_should_start_record = false;
static volatile bool s_should_start_babysound = false;
static volatile bool s_should_return_home = false;
static volatile bool s_should_toggle_record = false;  // 新增：录音按钮被点击

// 婴儿声音检测开关状态（默认开启）
static volatile bool s_babysound_switch_on = true;

// 发送状态
static volatile ui_send_status_t s_send_status = UI_SEND_STATUS_NONE;
static volatile int s_send_progress = 0;

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

// 主界面按钮事件回调
static void btn_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (btn == s_btn_fall_debug) {
        ESP_LOGI(TAG, "Button clicked: enter fall debug");
        s_should_start_fall_debug = true;
    } else if (btn == s_btn_record) {
        ESP_LOGI(TAG, "Button clicked: enter record");
        s_should_start_record = true;
    } else if (btn == s_btn_babysound) {
        ESP_LOGI(TAG, "Button clicked: enter babysound");
        s_should_start_babysound = true;
    }
}

// 录音按钮事件回调
static void record_btn_event_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Record button clicked, running=%d", audio_recorder_is_running());
    s_should_toggle_record = true;
}

// 婴儿声音检测开关回调
static void babysound_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    s_babysound_switch_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Babysound switch: %s", s_babysound_switch_on ? "ON" : "OFF");

    if (s_babysound_label_status) {
        if (s_babysound_switch_on) {
            lv_label_set_text(s_babysound_label_status, "Detection: ON");
            lv_obj_set_style_text_color(s_babysound_label_status, lv_color_hex(0x07E0), 0);
        } else {
            lv_label_set_text(s_babysound_label_status, "Detection: OFF");
            lv_obj_set_style_text_color(s_babysound_label_status, lv_color_hex(0xaaaaaa), 0);
        }
    }
}

// ==================== 创建界面 ====================

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
    lv_obj_set_size(s_btn_fall_debug, 320, 60);
    lv_obj_align(s_btn_fall_debug, LV_ALIGN_CENTER, 0, -80);
    lv_obj_set_style_bg_color(s_btn_fall_debug, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(s_btn_fall_debug, lv_color_hex(0x0f3460), LV_STATE_PRESSED);

    lv_obj_t *label_btn1 = lv_label_create(s_btn_fall_debug);
    lv_label_set_text(label_btn1, "Fall Detection Debug");
    lv_obj_set_style_text_color(label_btn1, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_btn1, &lv_font_montserrat_14, 0);
    lv_obj_center(label_btn1);

    lv_obj_add_event_cb(s_btn_fall_debug, btn_event_cb, LV_EVENT_CLICKED, NULL);

    s_btn_record = lv_btn_create(s_home_screen);
    lv_obj_set_size(s_btn_record, 320, 60);
    lv_obj_align(s_btn_record, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_btn_record, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(s_btn_record, lv_color_hex(0x0f3460), LV_STATE_PRESSED);

    lv_obj_t *label_btn2 = lv_label_create(s_btn_record);
    lv_label_set_text(label_btn2, "Voice Recorder");
    lv_obj_set_style_text_color(label_btn2, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_btn2, &lv_font_montserrat_14, 0);
    lv_obj_center(label_btn2);

    lv_obj_add_event_cb(s_btn_record, btn_event_cb, LV_EVENT_CLICKED, NULL);

    s_btn_babysound = lv_btn_create(s_home_screen);
    lv_obj_set_size(s_btn_babysound, 320, 60);
    lv_obj_align(s_btn_babysound, LV_ALIGN_CENTER, 0, 80);
    lv_obj_set_style_bg_color(s_btn_babysound, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(s_btn_babysound, lv_color_hex(0x0f3460), LV_STATE_PRESSED);

    lv_obj_t *label_btn3 = lv_label_create(s_btn_babysound);
    lv_label_set_text(label_btn3, "Baby Sound Detection");
    lv_obj_set_style_text_color(label_btn3, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_btn3, &lv_font_montserrat_14, 0);
    lv_obj_center(label_btn3);

    lv_obj_add_event_cb(s_btn_babysound, btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_scr_load(s_home_screen);
}

static void create_fall_debug_screen(void)
{
    s_fall_debug_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_fall_debug_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_size(s_fall_debug_screen, LCD_WIDTH, LCD_HEIGHT);

    lv_obj_t *label = lv_label_create(s_fall_debug_screen);
    lv_label_set_text(label, "Swipe right to return");
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *gesture_area = lv_obj_create(s_fall_debug_screen);
    lv_obj_set_size(gesture_area, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_bg_opa(gesture_area, LV_OPA_0, 0);
    lv_obj_set_style_border_width(gesture_area, 0, 0);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(gesture_area, LV_DIR_HOR);
}

static void create_record_screen(void)
{
    s_record_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_record_screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_size(s_record_screen, LCD_WIDTH, LCD_HEIGHT);

    lv_obj_t *label_title = lv_label_create(s_record_screen);
    lv_label_set_text(label_title, "Voice Recorder");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, 0);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 40);

    s_record_label_status = lv_label_create(s_record_screen);
    lv_label_set_text(s_record_label_status, "Ready");
    lv_obj_set_style_text_color(s_record_label_status, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_record_label_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_record_label_status, LV_ALIGN_CENTER, 0, -60);

    s_record_btn = lv_btn_create(s_record_screen);
    lv_obj_set_size(s_record_btn, 160, 50);
    lv_obj_align(s_record_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(0xff6b81), LV_STATE_PRESSED);

    s_record_label_btn = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_label_btn, "Start");
    lv_obj_set_style_text_color(s_record_label_btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_record_label_btn, &lv_font_montserrat_14, 0);
    lv_obj_center(s_record_label_btn);

    lv_obj_add_event_cb(s_record_btn, record_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // 发送状态显示
    s_record_label_send_status = lv_label_create(s_record_screen);
    lv_label_set_text(s_record_label_send_status, "");
    lv_obj_set_style_text_color(s_record_label_send_status, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_record_label_send_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_record_label_send_status, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *label_hint = lv_label_create(s_record_screen);
    lv_label_set_text(label_hint, "Swipe right to return");
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void create_babysound_screen(void)
{
    s_babysound_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_babysound_screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_size(s_babysound_screen, LCD_WIDTH, LCD_HEIGHT);

    lv_obj_t *label_title = lv_label_create(s_babysound_screen);
    lv_label_set_text(label_title, "Baby Sound Detection");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_14, 0);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 60);

    s_babysound_label_status = lv_label_create(s_babysound_screen);
    lv_label_set_text(s_babysound_label_status, "Detection: ON");
    lv_obj_set_style_text_color(s_babysound_label_status, lv_color_hex(0x07E0), 0);
    lv_obj_set_style_text_font(s_babysound_label_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_babysound_label_status, LV_ALIGN_CENTER, 0, -40);

    s_babysound_switch = lv_switch_create(s_babysound_screen);
    lv_obj_align(s_babysound_switch, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_babysound_switch, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_babysound_switch, lv_color_hex(0x07E0), LV_STATE_CHECKED);
    lv_obj_add_state(s_babysound_switch, LV_STATE_CHECKED);

    lv_obj_add_event_cb(s_babysound_switch, babysound_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *label_sw = lv_label_create(s_babysound_screen);
    lv_label_set_text(label_sw, "Auto Detection");
    lv_obj_set_style_text_color(label_sw, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_sw, &lv_font_montserrat_14, 0);
    lv_obj_align_to(label_sw, s_babysound_switch, LV_ALIGN_OUT_TOP_MID, 0, -10);

    lv_obj_t *label_hint = lv_label_create(s_babysound_screen);
    lv_label_set_text(label_hint, "Swipe right to return");
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

// 右滑返回手势检测
static void gesture_event_cb(lv_event_t *e)
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

    esp_lcd_panel_handle_t panel = lcd_display_get_panel();
    esp_lcd_panel_io_handle_t io = lcd_display_get_io();
    if (!panel || !io) {
        ESP_LOGE(TAG, "Failed to get panel or io handle");
        return ESP_FAIL;
    }

    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));

    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
        ESP_LV_ADAPTER_ROTATE_0
    );
    ESP_LOGI(TAG, "Required frame buffers for DOUBLE_FULL mode: %d", num_fbs);

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

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    ESP_ERROR_CHECK(touch_gt911_init());

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        create_home_screen();
        create_fall_debug_screen();
        create_record_screen();
        create_babysound_screen();

        lv_obj_add_event_cb(s_fall_debug_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(s_record_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(s_babysound_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);

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
        } else if (state == UI_STATE_RECORD) {
            lv_scr_load(s_record_screen);
            s_should_start_record = false;
            // 重置录音界面状态
            if (s_record_label_status) {
                lv_label_set_text(s_record_label_status, "Ready");
                lv_obj_set_style_text_color(s_record_label_status, lv_color_hex(0xaaaaaa), 0);
            }
            if (s_record_label_btn) {
                lv_label_set_text(s_record_label_btn, "Start");
            }
            if (s_record_label_send_status) {
                lv_label_set_text(s_record_label_send_status, "");
            }
        } else if (state == UI_STATE_BABYSOUND) {
            lv_scr_load(s_babysound_screen);
            s_should_start_babysound = false;
        }
        esp_lv_adapter_unlock();
    }
    ESP_LOGI(TAG, "State changed to: %s",
             state == UI_STATE_HOME ? "HOME" :
             state == UI_STATE_FALL_DEBUG ? "FALL_DEBUG" :
             state == UI_STATE_RECORD ? "RECORD" : "BABYSOUND");
}

void lvgl_ui_task_handler(void)
{
    if ((s_state == UI_STATE_FALL_DEBUG || s_state == UI_STATE_RECORD || s_state == UI_STATE_BABYSOUND)
        && s_should_return_home) {
        lvgl_ui_set_state(UI_STATE_HOME);
    }

    static bool s_was_recording = false;
    bool is_recording = audio_recorder_is_running();

    // 录音状态变化时更新UI
    if (s_was_recording && !is_recording) {
        // 录音结束
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_record_label_status) {
                lv_label_set_text(s_record_label_status, "Saved");
                lv_obj_set_style_text_color(s_record_label_status, lv_color_hex(0x07E0), 0);
            }
            if (s_record_label_btn) {
                lv_label_set_text(s_record_label_btn, "Start");
            }
            esp_lv_adapter_unlock();
        }
    }
    s_was_recording = is_recording;

    // 更新发送状态显示
    if (s_state == UI_STATE_RECORD && s_record_label_send_status) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            switch (s_send_status) {
            case UI_SEND_STATUS_SENDING:
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Sending... %d%%", s_send_progress);
                    lv_label_set_text(s_record_label_send_status, buf);
                    lv_obj_set_style_text_color(s_record_label_send_status, lv_color_hex(0xFFFF00), 0);
                }
                break;
            case UI_SEND_STATUS_SUCCESS:
                lv_label_set_text(s_record_label_send_status, "Send Success!");
                lv_obj_set_style_text_color(s_record_label_send_status, lv_color_hex(0x07E0), 0);
                break;
            case UI_SEND_STATUS_FAILED:
                lv_label_set_text(s_record_label_send_status, "Send Failed!");
                lv_obj_set_style_text_color(s_record_label_send_status, lv_color_hex(0xF800), 0);
                break;
            default:
                break;
            }
            esp_lv_adapter_unlock();
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
}

bool lvgl_ui_should_start_fall_debug(void)
{
    bool ret = s_should_start_fall_debug;
    s_should_start_fall_debug = false;
    return ret;
}

bool lvgl_ui_should_start_record(void)
{
    bool ret = s_should_start_record;
    s_should_start_record = false;
    return ret;
}

bool lvgl_ui_should_start_babysound(void)
{
    bool ret = s_should_start_babysound;
    s_should_start_babysound = false;
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

bool lvgl_ui_babysound_switch_is_on(void)
{
    return s_babysound_switch_on;
}

void lvgl_ui_set_babysound_switch(bool on)
{
    s_babysound_switch_on = on;
    if (s_babysound_switch) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (on) {
                lv_obj_add_state(s_babysound_switch, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_babysound_switch, LV_STATE_CHECKED);
            }
            esp_lv_adapter_unlock();
        }
    }
}

bool lvgl_ui_should_toggle_record(void)
{
    bool ret = s_should_toggle_record;
    return ret;
}

void lvgl_ui_clear_toggle_record(void)
{
    s_should_toggle_record = false;
}

void lvgl_ui_set_send_status(ui_send_status_t status)
{
    s_send_status = status;
}

void lvgl_ui_set_send_progress(int progress)
{
    s_send_progress = progress;
}