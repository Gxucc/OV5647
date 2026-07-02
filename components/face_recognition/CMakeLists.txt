idf_component_register(
    SRCS "face_recognition.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-dl human_face_recognition human_face_detect
)