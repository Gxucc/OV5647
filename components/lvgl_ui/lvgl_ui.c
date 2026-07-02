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

// 状态变化回调
static lvgl_ui_state_change_cb_t s_state_change_cb = NULL;

// ============ 颜色定义（Material Dark） ============
#define COLOR_BG            0x121212
#define COLOR_SURFACE       0x1E1E1E
#define COLOR_SURFACE_LIGH  0x2C2C2C
#define COLOR_PRIMARY       0x4CAF50
#define COLOR_PRIMARY_DARK  0x388E3C
#define COLOR_ACCENT        0xE91E63
#define COLOR_TEXT_PRIMARY  0xFFFFFF
#define COLOR_TEXT_SECOND   0xB0B0B0
#define COLOR_TEXT_DISABLED 0x666666
#define COLOR_DIVIDER       0x2C2C2C
#define COLOR_TOAST_BG      0x323232

// 主界面对象
static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_appbar_title = NULL;

// 列表项（4个功能入口）
typedef struct {
    lv_obj_t *container;
    lv_obj_t *icon_bg;
    lv_obj_t *icon_label;
    lv_obj_t *title_label;
    lv_obj_t *arrow_label;
    lv_obj_t *badge;
} list_item_t;

static list_item_t s_item_fall = {0};
static list_item_t s_item_record = {0};
static list_item_t s_item_baby = {0};
static list_item_t s_item_face = {0};

// 子界面对象
static lv_obj_t *s_fall_debug_screen = NULL;
static lv_obj_t *s_record_screen = NULL;
static lv_obj_t *s_record_btn = NULL;
static lv_obj_t *s_record_label_status = NULL;
static lv_obj_t *s_record_label_btn = NULL;
static lv_obj_t *s_record_label_send_status = NULL;

static lv_obj_t *s_babysound_screen = NULL;
static lv_obj_t *s_babysound_switch = NULL;
static lv_obj_t *s_babysound_label_status = NULL;

static lv_obj_t *s_face_recognition_screen = NULL;

// Toast 对象
static lv_obj_t *s_toast_container = NULL;
static lv_obj_t *s_toast_label = NULL;
static lv_obj_t *s_toast_icon = NULL;
static lv_timer_t *s_toast_timer = NULL;

// 网络状态指示器
static lv_obj_t *s_net_indicator = NULL;

// 标志位
static volatile bool s_should_start_fall_debug = false;
static volatile bool s_should_start_record = false;
static volatile bool s_should_start_babysound = false;
static volatile bool s_should_start_face_recognition = false;
static volatile bool s_should_return_home = false;
static volatile bool s_should_toggle_record = false;

// 婴儿声音检测开关状态（默认开启）
static volatile bool s_babysound_switch_on = false;

// 发送状态
static volatile ui_send_status_t s_send_status = UI_SEND_STATUS_NONE;
static volatile int s_send_progress = 0;

// ==================== 工具函数 ====================

static lv_color_t hex_color(uint32_t hex)
{
    return lv_color_hex(hex);
}

// 创建圆角卡片容器
static lv_obj_t* create_card(lv_obj_t *parent, int32_t w, int32_t h, int32_t radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, hex_color(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    return card;
}

// 创建圆形图标背景
static lv_obj_t* create_circle_icon(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_set_size(circle, 44, 44);
    lv_obj_set_style_bg_color(circle, hex_color(color), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(circle, 22, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(circle);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);

    return circle;
}

// ==================== 回调注册 ====================

void lvgl_ui_register_state_change_cb(lvgl_ui_state_change_cb_t cb)
{
    s_state_change_cb = cb;
}

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

// ==================== 列表项点击事件 ====================

static void list_item_event_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);

    if (target == s_item_fall.container || target == s_item_fall.icon_bg || 
        target == s_item_fall.title_label || target == s_item_fall.arrow_label) {
        ESP_LOGI(TAG, "List item clicked: Fall Detection");
        s_should_start_fall_debug = true;
    } else if (target == s_item_record.container || target == s_item_record.icon_bg ||
               target == s_item_record.title_label || target == s_item_record.arrow_label) {
        ESP_LOGI(TAG, "List item clicked: Voice Recorder");
        s_should_start_record = true;
    } else if (target == s_item_baby.container || target == s_item_baby.icon_bg ||
               target == s_item_baby.title_label || target == s_item_baby.arrow_label) {
        ESP_LOGI(TAG, "List item clicked: Baby Sound");
        s_should_start_babysound = true;
    } else if (target == s_item_face.container || target == s_item_face.icon_bg ||
               target == s_item_face.title_label || target == s_item_face.arrow_label) {
        ESP_LOGI(TAG, "List item clicked: Face Recognition");
        s_should_start_face_recognition = true;
    }
}

// 列表项按压样式变化
static void list_item_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *container = lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_color(container, hex_color(COLOR_SURFACE_LIGH), 0);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(container, hex_color(COLOR_SURFACE), 0);
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
            lv_obj_set_style_text_color(s_babysound_label_status, hex_color(COLOR_PRIMARY), 0);
        } else {
            lv_label_set_text(s_babysound_label_status, "Detection: OFF");
            lv_obj_set_style_text_color(s_babysound_label_status, hex_color(COLOR_TEXT_DISABLED), 0);
        }
    }
}

// ==================== Toast 功能 ====================

static void toast_timer_cb(lv_timer_t *timer)
{
    if (s_toast_container) {
        lv_obj_set_style_bg_opa(s_toast_container, LV_OPA_0, 0);
        lv_obj_add_flag(s_toast_container, LV_OBJ_FLAG_HIDDEN);
    }
    s_toast_timer = NULL;
}

void lvgl_ui_show_alarm_toast(ui_alarm_type_t type, const char *detail)
{
    if (!s_toast_container) return;

    const char *icon = "";
    const char *text = "";
    uint32_t color = COLOR_ACCENT;

    switch (type) {
        case UI_ALARM_FALL:
            icon = "!";
            text = "Fall Detected!";
            color = 0xFF5722;  // 橙色
            break;
        case UI_ALARM_BABY:
            icon = "B";
            text = "Baby Sound!";
            color = 0xFF9800;  // 琥珀色
            break;
        case UI_ALARM_STRANGER:
            icon = "?";
            text = "Stranger Alert!";
            color = COLOR_ACCENT;  // 粉红
            break;
        case UI_ALARM_NET_LOST:
            icon = "X";
            text = "Network Lost";
            color = 0xF44336;  // 红色
            break;
        default:
            return;
    }

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        // 更新图标
        if (s_toast_icon) {
            lv_label_set_text(s_toast_icon, icon);
            lv_obj_set_style_bg_color(lv_obj_get_parent(s_toast_icon), hex_color(color), 0);
        }
        // 更新文字
        if (s_toast_label) {
            char buf[64];
            if (detail && strlen(detail) > 0) {
                snprintf(buf, sizeof(buf), "%s %s", text, detail);
            } else {
                strncpy(buf, text, sizeof(buf));
            }
            lv_label_set_text(s_toast_label, buf);
        }

        lv_obj_clear_flag(s_toast_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_toast_container, LV_OPA_80, 0);
        lv_obj_set_style_text_opa(s_toast_container, LV_OPA_COVER, 0);

        esp_lv_adapter_unlock();
    }

    // 重置定时器
    if (s_toast_timer) {
        lv_timer_reset(s_toast_timer);
    } else {
        s_toast_timer = lv_timer_create(toast_timer_cb, 3000, NULL);
        lv_timer_set_repeat_count(s_toast_timer, 1);
    }

    ESP_LOGI(TAG, "Toast shown: %s", text);
}

void lvgl_ui_show_net_status(bool connected)
{
    if (!s_net_indicator) return;

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (connected) {
            lv_obj_set_style_bg_color(s_net_indicator, hex_color(COLOR_PRIMARY), 0);
        } else {
            lv_obj_set_style_bg_color(s_net_indicator, hex_color(0xF44336), 0);
        }
        esp_lv_adapter_unlock();
    }
}

// ==================== 创建主界面 ====================

static void create_home_screen(void)
{
    s_home_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_home_screen, hex_color(COLOR_BG), 0);
    lv_obj_set_size(s_home_screen, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_pad_all(s_home_screen, 0, 0);
    lv_obj_set_scrollbar_mode(s_home_screen, LV_SCROLLBAR_MODE_OFF);

    // ========== 顶部 AppBar ==========
    lv_obj_t *appbar = lv_obj_create(s_home_screen);
    lv_obj_set_size(appbar, LCD_WIDTH, 56);
    lv_obj_set_style_bg_color(appbar, hex_color(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(appbar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(appbar, 0, 0);
    lv_obj_set_style_border_width(appbar, 0, 0);
    lv_obj_set_style_pad_all(appbar, 0, 0);
    lv_obj_align(appbar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(appbar, LV_OBJ_FLAG_SCROLLABLE);

    s_appbar_title = lv_label_create(appbar);
    lv_label_set_text(s_appbar_title, "Family Care");
    lv_obj_set_style_text_color(s_appbar_title, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_appbar_title, &lv_font_montserrat_16, 0);
    lv_obj_align(s_appbar_title, LV_ALIGN_LEFT_MID, 20, 0);

    // 网络状态指示器（小圆点）
    s_net_indicator = lv_obj_create(appbar);
    lv_obj_set_size(s_net_indicator, 8, 8);
    lv_obj_set_style_bg_color(s_net_indicator, hex_color(COLOR_PRIMARY), 0);
    lv_obj_set_style_bg_opa(s_net_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_net_indicator, 4, 0);
    lv_obj_set_style_border_width(s_net_indicator, 0, 0);
    lv_obj_set_style_pad_all(s_net_indicator, 0, 0);
    lv_obj_align(s_net_indicator, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_clear_flag(s_net_indicator, LV_OBJ_FLAG_CLICKABLE);

    // ========== 功能列表 ==========
    lv_obj_t *list_container = lv_obj_create(s_home_screen);
    lv_obj_set_size(list_container, LCD_WIDTH - 32, 360);
    lv_obj_set_style_bg_opa(list_container, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_pad_all(list_container, 0, 0);
    lv_obj_set_style_pad_row(list_container, 8, 0);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(list_container, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_SCROLLABLE);

    // 创建4个列表项
    struct {
        list_item_t *item;
        const char *icon;
        const char *title;
        uint32_t icon_color;
    } items[4] = {
        {&s_item_fall, "F", "Fall Detection", 0xFF5722},
        {&s_item_record, "R", "Voice Recorder", 0x2196F3},
        {&s_item_baby, "B", "Baby Monitor", 0xFF9800},
        {&s_item_face, "F", "Face Recognition", 0x9C27B0},
    };

    for (int i = 0; i < 4; i++) {
        list_item_t *item = items[i].item;

        // 卡片容器
        item->container = create_card(list_container, LCD_WIDTH - 32, 64, 12);
        lv_obj_set_style_pad_all(item->container, 12, 0);
        lv_obj_set_flex_flow(item->container, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item->container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item->container, 16, 0);
        lv_obj_add_flag(item->container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(item->container, LV_OBJ_FLAG_SCROLLABLE);

        // 添加点击事件
        lv_obj_add_event_cb(item->container, list_item_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(item->container, list_item_press_cb, LV_EVENT_ALL, NULL);

        // 圆形图标
        item->icon_bg = create_circle_icon(item->container, items[i].icon, items[i].icon_color);

        // 标题
        item->title_label = lv_label_create(item->container);
        lv_label_set_text(item->title_label, items[i].title);
        lv_obj_set_style_text_color(item->title_label, hex_color(COLOR_TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(item->title_label, &lv_font_montserrat_16, 0);
        lv_obj_set_flex_grow(item->title_label, 1);
        lv_obj_add_flag(item->title_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item->title_label, list_item_event_cb, LV_EVENT_CLICKED, NULL);

        // 右箭头
        item->arrow_label = lv_label_create(item->container);
        lv_label_set_text(item->arrow_label, ">");
        lv_obj_set_style_text_color(item->arrow_label, hex_color(COLOR_TEXT_DISABLED), 0);
        lv_obj_set_style_text_font(item->arrow_label, &lv_font_montserrat_16, 0);
        lv_obj_add_flag(item->arrow_label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item->arrow_label, list_item_event_cb, LV_EVENT_CLICKED, NULL);

        // Badge（小红点，默认隐藏）
        item->badge = lv_obj_create(item->container);
        lv_obj_set_size(item->badge, 8, 8);
        lv_obj_set_style_bg_color(item->badge, hex_color(COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(item->badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item->badge, 4, 0);
        lv_obj_set_style_border_width(item->badge, 0, 0);
        lv_obj_set_style_pad_all(item->badge, 0, 0);
        lv_obj_add_flag(item->badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(item->badge, LV_OBJ_FLAG_CLICKABLE);
    }

    // ========== Toast 容器（初始隐藏） ==========
    s_toast_container = lv_obj_create(s_home_screen);
    lv_obj_set_size(s_toast_container, LCD_WIDTH - 64, 48);
    lv_obj_set_style_bg_color(s_toast_container, hex_color(COLOR_TOAST_BG), 0);
    lv_obj_set_style_bg_opa(s_toast_container, LV_OPA_0, 0);
    lv_obj_set_style_radius(s_toast_container, 24, 0);
    lv_obj_set_style_border_width(s_toast_container, 0, 0);
    lv_obj_set_style_pad_all(s_toast_container, 8, 0);
    lv_obj_set_style_pad_left(s_toast_container, 16, 0);
    lv_obj_set_style_pad_right(s_toast_container, 16, 0);
    lv_obj_align(s_toast_container, LV_ALIGN_BOTTOM_MID, 0, -32);
    lv_obj_set_flex_flow(s_toast_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_toast_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_toast_container, 12, 0);
    lv_obj_add_flag(s_toast_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_toast_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_toast_container, LV_OBJ_FLAG_CLICKABLE);

    // Toast 图标
    lv_obj_t *toast_icon_bg = lv_obj_create(s_toast_container);
    lv_obj_set_size(toast_icon_bg, 28, 28);
    lv_obj_set_style_bg_color(toast_icon_bg, hex_color(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(toast_icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast_icon_bg, 14, 0);
    lv_obj_set_style_border_width(toast_icon_bg, 0, 0);
    lv_obj_set_style_pad_all(toast_icon_bg, 0, 0);
    lv_obj_clear_flag(toast_icon_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(toast_icon_bg, LV_OBJ_FLAG_SCROLLABLE);

    s_toast_icon = lv_label_create(toast_icon_bg);
    lv_label_set_text(s_toast_icon, "!");
    lv_obj_set_style_text_color(s_toast_icon, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_toast_icon, &lv_font_montserrat_14, 0);
    lv_obj_center(s_toast_icon);

    // Toast 文字
    s_toast_label = lv_label_create(s_toast_container);
    lv_label_set_text(s_toast_label, "");
    lv_obj_set_style_text_color(s_toast_label, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, 0);
    lv_obj_set_flex_grow(s_toast_label, 1);

    lv_scr_load(s_home_screen);
}

// ==================== 创建子界面（带 AppBar） ====================

static lv_obj_t* create_appbar_screen(const char *title)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, hex_color(COLOR_BG), 0);
    lv_obj_set_size(screen, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // AppBar
    lv_obj_t *appbar = lv_obj_create(screen);
    lv_obj_set_size(appbar, LCD_WIDTH, 56);
    lv_obj_set_style_bg_color(appbar, hex_color(COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(appbar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(appbar, 0, 0);
    lv_obj_set_style_border_width(appbar, 0, 0);
    lv_obj_set_style_pad_all(appbar, 0, 0);
    lv_obj_align(appbar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(appbar, LV_OBJ_FLAG_SCROLLABLE);

    // 返回按钮
    lv_obj_t *btn_back = lv_btn_create(appbar);
    lv_obj_set_size(btn_back, 48, 48);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_back, 24, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(btn_back, (lv_event_cb_t)lvgl_ui_set_return_home, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, "<");
    lv_obj_set_style_text_color(label_back, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_16, 0);
    lv_obj_center(label_back);

    // 标题
    lv_obj_t *label_title = lv_label_create(appbar);
    lv_label_set_text(label_title, title);
    lv_obj_set_style_text_color(label_title, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(label_title, &lv_font_montserrat_16, 0);
    lv_obj_align(label_title, LV_ALIGN_CENTER, 0, 0);

    return screen;
}

static void create_fall_debug_screen(void)
{
    s_fall_debug_screen = create_appbar_screen("Fall Detection");

    // 状态提示
    lv_obj_t *label = lv_label_create(s_fall_debug_screen);
    lv_label_set_text(label, "Swipe right to return");
    lv_obj_set_style_text_color(label, hex_color(COLOR_TEXT_SECOND), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 72);

    // 手势区域
    lv_obj_t *gesture_area = lv_obj_create(s_fall_debug_screen);
    lv_obj_set_size(gesture_area, LCD_WIDTH, LCD_HEIGHT - 56);
    lv_obj_set_style_bg_opa(gesture_area, LV_OPA_0, 0);
    lv_obj_set_style_border_width(gesture_area, 0, 0);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(gesture_area, LV_DIR_HOR);
    lv_obj_align(gesture_area, LV_ALIGN_TOP_MID, 0, 56);
}

static void create_record_screen(void)
{
    s_record_screen = create_appbar_screen("Voice Recorder");

    // 状态文字
    s_record_label_status = lv_label_create(s_record_screen);
    lv_label_set_text(s_record_label_status, "Ready");
    lv_obj_set_style_text_color(s_record_label_status, hex_color(COLOR_TEXT_SECOND), 0);
    lv_obj_set_style_text_font(s_record_label_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_record_label_status, LV_ALIGN_CENTER, 0, -80);

    // FAB 圆形按钮
    s_record_btn = lv_btn_create(s_record_screen);
    lv_obj_set_size(s_record_btn, 80, 80);
    lv_obj_set_style_bg_color(s_record_btn, hex_color(COLOR_PRIMARY), 0);
    lv_obj_set_style_bg_color(s_record_btn, hex_color(COLOR_PRIMARY_DARK), LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_record_btn, 40, 0);
    lv_obj_set_style_border_width(s_record_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_record_btn, 16, 0);
    lv_obj_set_style_shadow_color(s_record_btn, hex_color(COLOR_PRIMARY), 0);
    lv_obj_set_style_shadow_opa(s_record_btn, LV_OPA_30, 0);
    lv_obj_align(s_record_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(s_record_btn, record_btn_event_cb, LV_EVENT_CLICKED, NULL);

    s_record_label_btn = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_label_btn, "R");
    lv_obj_set_style_text_color(s_record_label_btn, hex_color(COLOR_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_record_label_btn, &lv_font_montserrat_16, 0);
    lv_obj_center(s_record_label_btn);

    // 发送状态
    s_record_label_send_status = lv_label_create(s_record_screen);
    lv_label_set_text(s_record_label_send_status, "");
    lv_obj_set_style_text_color(s_record_label_send_status, hex_color(COLOR_TEXT_SECOND), 0);
    lv_obj_set_style_text_font(s_record_label_send_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_record_label_send_status, LV_ALIGN_CENTER, 0, 80);

    // 提示
    lv_obj_t *label_hint = lv_label_create(s_record_screen);
    lv_label_set_text(label_hint, "Tap button to record");
    lv_obj_set_style_text_color(label_hint, hex_color(COLOR_TEXT_DISABLED), 0);
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -40);
}

static void create_babysound_screen(void)
{
    s_babysound_screen = create_appbar_screen("Baby Monitor");

    // 状态文字
    s_babysound_label_status = lv_label_create(s_babysound_screen);
    lv_label_set_text(s_babysound_label_status, "Detection: OFF");
    lv_obj_set_style_text_color(s_babysound_label_status, hex_color(COLOR_TEXT_DISABLED), 0);
    lv_obj_set_style_text_font(s_babysound_label_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_babysound_label_status, LV_ALIGN_CENTER, 0, -60);

    // 大 Switch
    s_babysound_switch = lv_switch_create(s_babysound_screen);
    lv_obj_set_size(s_babysound_switch, 80, 40);
    lv_obj_align(s_babysound_switch, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_bg_color(s_babysound_switch, hex_color(COLOR_SURFACE_LIGH), 0);
    lv_obj_set_style_bg_color(s_babysound_switch, hex_color(COLOR_PRIMARY), LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_babysound_switch, 20, 0);
    lv_obj_set_style_radius(s_babysound_switch, 20, LV_PART_INDICATOR);
    // lv_obj_add_state(s_babysound_switch, LV_STATE_CHECKED);

    lv_obj_add_event_cb(s_babysound_switch, babysound_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // 标签
    lv_obj_t *label_sw = lv_label_create(s_babysound_screen);
    lv_label_set_text(label_sw, "Auto Detection");
    lv_obj_set_style_text_color(label_sw, hex_color(COLOR_TEXT_SECOND), 0);
    lv_obj_set_style_text_font(label_sw, &lv_font_montserrat_14, 0);
    lv_obj_align_to(label_sw, s_babysound_switch, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    // 提示
    lv_obj_t *label_hint = lv_label_create(s_babysound_screen);
    lv_label_set_text(label_hint, "Swipe right to return");
    lv_obj_set_style_text_color(label_hint, hex_color(COLOR_TEXT_DISABLED), 0);
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -40);
}

static void create_face_recognition_screen(void)
{
    s_face_recognition_screen = create_appbar_screen("Face Recognition");

    // 状态提示
    lv_obj_t *label = lv_label_create(s_face_recognition_screen);
    lv_label_set_text(label, "Swipe right to return");
    lv_obj_set_style_text_color(label, hex_color(COLOR_TEXT_SECOND), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 72);

    // 手势区域
    lv_obj_t *gesture_area = lv_obj_create(s_face_recognition_screen);
    lv_obj_set_size(gesture_area, LCD_WIDTH, LCD_HEIGHT - 56);
    lv_obj_set_style_bg_opa(gesture_area, LV_OPA_0, 0);
    lv_obj_set_style_border_width(gesture_area, 0, 0);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(gesture_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(gesture_area, LV_DIR_HOR);
    lv_obj_align(gesture_area, LV_ALIGN_TOP_MID, 0, 56);
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
        create_face_recognition_screen();

        lv_obj_add_event_cb(s_fall_debug_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(s_record_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(s_babysound_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);
        lv_obj_add_event_cb(s_face_recognition_screen, gesture_event_cb, LV_EVENT_GESTURE, NULL);

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
    ui_state_t prev_state = s_state;
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
            if (s_record_label_status) {
                lv_label_set_text(s_record_label_status, "Ready");
                lv_obj_set_style_text_color(s_record_label_status, hex_color(COLOR_TEXT_SECOND), 0);
            }
            if (s_record_label_btn) {
                lv_label_set_text(s_record_label_btn, "R");
                lv_obj_set_style_bg_color(s_record_btn, hex_color(COLOR_PRIMARY), 0);
            }
            if (s_record_label_send_status) {
                lv_label_set_text(s_record_label_send_status, "");
            }
        } else if (state == UI_STATE_BABYSOUND) {
            lv_scr_load(s_babysound_screen);
            s_should_start_babysound = false;
        } else if (state == UI_STATE_FACE_RECOGNITION) {
            lv_scr_load(s_face_recognition_screen);
            s_should_start_face_recognition = false;
        }
        esp_lv_adapter_unlock();
    }

    // 触发状态变化回调
    if (s_state_change_cb && prev_state != state) {
        s_state_change_cb(prev_state, state);
    }

    ESP_LOGI(TAG, "State changed: %s -> %s",
             prev_state == UI_STATE_HOME ? "HOME" :
             prev_state == UI_STATE_FALL_DEBUG ? "FALL_DEBUG" :
             prev_state == UI_STATE_RECORD ? "RECORD" :
             prev_state == UI_STATE_BABYSOUND ? "BABYSOUND" : "FACE_RECOGNITION",
             state == UI_STATE_HOME ? "HOME" :
             state == UI_STATE_FALL_DEBUG ? "FALL_DEBUG" :
             state == UI_STATE_RECORD ? "RECORD" :
             state == UI_STATE_BABYSOUND ? "BABYSOUND" : "FACE_RECOGNITION");
}

void lvgl_ui_task_handler(void)
{
    if ((s_state == UI_STATE_FALL_DEBUG || s_state == UI_STATE_RECORD || 
         s_state == UI_STATE_BABYSOUND || s_state == UI_STATE_FACE_RECOGNITION)
        && s_should_return_home) {
        lvgl_ui_set_state(UI_STATE_HOME);
    }

    static bool s_was_recording = false;
    bool is_recording = audio_recorder_is_running();

    if (s_was_recording && !is_recording) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            if (s_record_label_status) {
                lv_label_set_text(s_record_label_status, "Saved");
                lv_obj_set_style_text_color(s_record_label_status, hex_color(COLOR_PRIMARY), 0);
            }
            if (s_record_label_btn) {
                lv_label_set_text(s_record_label_btn, "R");
                lv_obj_set_style_bg_color(s_record_btn, hex_color(COLOR_PRIMARY), 0);
            }
            esp_lv_adapter_unlock();
        }
    }
    s_was_recording = is_recording;

    if (s_state == UI_STATE_RECORD && s_record_label_send_status) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            switch (s_send_status) {
            case UI_SEND_STATUS_SENDING:
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "Sending... %d%%", s_send_progress);
                    lv_label_set_text(s_record_label_send_status, buf);
                    lv_obj_set_style_text_color(s_record_label_send_status, hex_color(0xFFFF00), 0);
                }
                break;
            case UI_SEND_STATUS_SUCCESS:
                lv_label_set_text(s_record_label_send_status, "Send Success!");
                lv_obj_set_style_text_color(s_record_label_send_status, hex_color(COLOR_PRIMARY), 0);
                break;
            case UI_SEND_STATUS_FAILED:
                lv_label_set_text(s_record_label_send_status, "Send Failed!");
                lv_obj_set_style_text_color(s_record_label_send_status, hex_color(0xF44336), 0);
                break;
            default:
                break;
            }
            esp_lv_adapter_unlock();
        }
    }

    // 录音中按钮变红色
    if (s_state == UI_STATE_RECORD && is_recording && s_record_btn) {
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            lv_obj_set_style_bg_color(s_record_btn, hex_color(COLOR_ACCENT), 0);
            if (s_record_label_btn) {
                lv_label_set_text(s_record_label_btn, "S");
            }
            if (s_record_label_status) {
                lv_label_set_text(s_record_label_status, "Recording...");
                lv_obj_set_style_text_color(s_record_label_status, hex_color(COLOR_ACCENT), 0);
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

bool lvgl_ui_should_start_face_recognition(void)
{
    bool ret = s_should_start_face_recognition;
    s_should_start_face_recognition = false;
    return ret;
}

void lvgl_ui_clear_start_face_recognition(void)
{
    s_should_start_face_recognition = false;
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