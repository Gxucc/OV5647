idf_component_register(
    SRCS "person_detect.cpp"   
    INCLUDE_DIRS "include"
    REQUIRES pedestrian_detect esp-dl
)