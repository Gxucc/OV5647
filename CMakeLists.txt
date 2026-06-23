cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sample_project)

# ========== 模型自动烧录 ==========
set(image_file ${CMAKE_SOURCE_DIR}/components/fall_classifier/model/fall_classifier.espdl)
partition_table_get_partition_info(size "--partition-name model" "size")
if("${size}")
    esptool_py_flash_to_partition(flash "model" "${image_file}")
endif()