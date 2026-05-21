idf_component_register(
    SRCS "yolo11_pose_wrapper.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp-dl coco_pose
)