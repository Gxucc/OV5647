idf_component_register(
    SRCS "babysound_detector.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-dl audio_recorder
)