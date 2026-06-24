# components/audio_recorder/CMakeLists.txt

idf_component_register(
    SRCS "audio_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver          # I2S driver
    PRIV_REQUIRES
        fatfs           # 文件操作 (fopen, fwrite)
        sdmmc           # SD卡 (可选，如果保存到SD卡需要)
        esp_codec_dev
        esp_timer
)