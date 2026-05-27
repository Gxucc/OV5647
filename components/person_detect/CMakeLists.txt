idf_component_register(
    SRCS "person_detect.cpp" "draw_box.c"   # ← 注意 .cpp
    INCLUDE_DIRS "include"
    REQUIRES pedestrian_detect esp-dl
)