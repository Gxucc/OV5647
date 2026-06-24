idf_component_register(
    SRCS "lvgl_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        lcd_display
        esp_lvgl_adapter
        touch_gt911
        audio_recorder
)