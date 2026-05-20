idf_component_register(
    SRCS "net_service.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        freertos
        esp_wifi
        esp_wifi_remote
        esp_http_server
        esp_timer
        nvs_flash
        esp_event
        esp_netif
        cam_net
)