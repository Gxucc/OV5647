idf_component_register(
    SRCS "fall_detector.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-dl esp_common
    EMBED_FILES "model/yolo11n-pose-192.espdl"
)