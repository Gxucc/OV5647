cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sample_project)

# ========== 模型自动烧录 ==========
# 跌倒检测模型
set(fall_model ${CMAKE_SOURCE_DIR}/components/fall_classifier/model/fall_classifier.espdl)
partition_table_get_partition_info(fall_size "--partition-name model_fall" "size")
if("${fall_size}")
    esptool_py_flash_to_partition(flash "model_fall" "${fall_model}")
endif()

# 婴儿哭声检测模型
set(audio_model ${CMAKE_SOURCE_DIR}/components/babysound_detector/model/baby_sound_detector.espdl)
partition_table_get_partition_info(audio_size "--partition-name model_audio" "size")
if("${audio_size}")
    esptool_py_flash_to_partition(flash "model_audio" "${audio_model}")
endif()