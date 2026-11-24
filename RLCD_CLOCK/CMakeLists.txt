# 配置天气时钟 ESP-IDF 工程版本、项目名和额外组件目录。
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "v1.6.18")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS
    components/ExternLib/SensorLib
    components/ExternLib/esp_codec_dev
    components/ExternLib/codec_board)
project(weather_clock)

# 每次构建前跑引擎层门面守卫：上层只许 include engine_api.h，
# 引擎不许反向 include ui/ 的头。IDF 的 REQUIRES 不管 include 可见性，
# 写错方向照样编得过，所以必须静态扫描兜底。
# 只读检查，不改任何构建产物；违规直接让构建失败。
add_custom_target(weather_clock_dep_guard ALL
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH= python3
            ${CMAKE_SOURCE_DIR}/tools/check_deps.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "引擎层门面守卫 (tools/check_deps.py)"
    VERBATIM
)
add_dependencies(${CMAKE_PROJECT_NAME}.elf weather_clock_dep_guard)

# Keep the object arena outside scarce internal/DMA RAM, matching SDL's budget.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/components/app_bsp")
target_compile_definitions(${lvgl_lib} PRIVATE
    LV_MEM_SIZE=262144
    LV_MEM_POOL_INCLUDE="lvgl_memory_pool.h"
    LV_MEM_POOL_ALLOC=weather_clock_lvgl_pool)
