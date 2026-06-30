idf_component_register(SRCS "audio_mqtt_sender.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi mqtt nvs_flash esp_audio_codec)