# Platform selection is centralized here. Each board owns only its concrete
# wiring and startup sources; reusable transports, drivers, and renderers live
# in their corresponding layer CMake files.
include("${CMAKE_CURRENT_LIST_DIR}/audio/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/haptics/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/random/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/wifi/CMakeLists.txt")
list(APPEND micropixel_sources "platform/platform.cpp")

# IDF_TARGET is available during early component expansion while project
# Kconfig values are not. Resolve the narrow SoC dependency set first, then
# select exactly one board implementation below.
if(IDF_TARGET STREQUAL "esp32p4")
    include("${CMAKE_CURRENT_LIST_DIR}/targets/esp32p4/CMakeLists.txt")
elseif(IDF_TARGET STREQUAL "esp32s31")
    include("${CMAKE_CURRENT_LIST_DIR}/targets/esp32s31/CMakeLists.txt")
elseif(IDF_TARGET STREQUAL "esp32s3")
    include("${CMAKE_CURRENT_LIST_DIR}/targets/esp32s3/CMakeLists.txt")
else()
    message(FATAL_ERROR "MicroPixel does not support IDF target ${IDF_TARGET}")
endif()

if(CMAKE_BUILD_EARLY_EXPANSION)
    # Kconfig values are intentionally unavailable while ESP-IDF discovers
    # component requirements. The dependency union above is authoritative in
    # this pass; use the minimal source set only to keep script evaluation valid.
    include("${CMAKE_CURRENT_LIST_DIR}/boards/null/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_METALIO_CLAW4)
    include("${CMAKE_CURRENT_LIST_DIR}/adapters/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/buses/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/lvgl/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/metalio-claw4/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_ESP_MOSAICO)
    include("${CMAKE_CURRENT_LIST_DIR}/adapters/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/buses/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/gpio/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/lvgl/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/esp-mosaico/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_ESP32_S3_BOX_3)
    include("${CMAKE_CURRENT_LIST_DIR}/adapters/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/buses/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/gpio/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/lvgl/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/esp32-s3-common/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/esp32-s3-box-3/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_SZPI_ESP32S3)
    include("${CMAKE_CURRENT_LIST_DIR}/adapters/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/buses/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/gpio/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/lvgl/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/esp32-s3-common/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/szpi-esp32s3/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_NULL)
    include("${CMAKE_CURRENT_LIST_DIR}/boards/null/CMakeLists.txt")
else()
    message(FATAL_ERROR "MicroPixel requires exactly one configured board")
endif()
