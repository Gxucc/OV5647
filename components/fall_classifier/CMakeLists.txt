idf_component_register(
    SRCS "fall_classifier.cpp"
    INCLUDE_DIRS "include"
    REQUIRES 
        esp_timer
        esp-dl
)