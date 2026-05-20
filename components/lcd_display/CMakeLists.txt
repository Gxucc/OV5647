idf_component_register(
    SRCS "lcd_display.c"
    INCLUDE_DIRS "include"
    REQUIRES 
    esp_lcd 
    esp_lcd_ek79007  
    driver 
    esp_psram 
    esp_common 
    esp_driver_gpio heap 
    esp_mm 
    log 
    )
