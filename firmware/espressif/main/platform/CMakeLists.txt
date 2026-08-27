# Platform selection is centralized here. Each board owns only its concrete
# wiring and startup sources; reusable transports, drivers, and renderers live
# in their corresponding layer CMake files.
include("${CMAKE_CURRENT_LIST_DIR}/common/CMakeLists.txt")

list(APPEND micropixel_requires esp_hw_support)

# ESP-IDF expands component requirements before project Kconfig values are
# available, so this component declares the dependency union for all currently
# supported ESP32-P4 boards. Board source selection below remains exclusive.
list(APPEND micropixel_requires
    esp_driver_dma esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_jpeg esp_driver_ledc
    esp_driver_ppa esp_driver_uart esp_driver_usb_serial_jtag
    button esp_event esp_lcd esp_lvgl_adapter esp_lcd_touch_gt911 esp_netif esp_timer esp_wifi lvgl__lvgl
)

if(CMAKE_BUILD_EARLY_EXPANSION)
    # Kconfig values are intentionally unavailable while ESP-IDF discovers
    # component requirements. The dependency union above is authoritative in
    # this pass; use the minimal source set only to keep script evaluation valid.
    include("${CMAKE_CURRENT_LIST_DIR}/boards/null/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_METALIO_CLAW4)
    include("${CMAKE_CURRENT_LIST_DIR}/drivers/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/lvgl/CMakeLists.txt")
    include("${CMAKE_CURRENT_LIST_DIR}/boards/metalio-claw4/CMakeLists.txt")
elseif(CONFIG_MICROPIXEL_BOARD_NULL)
    include("${CMAKE_CURRENT_LIST_DIR}/boards/null/CMakeLists.txt")
else()
    message(FATAL_ERROR "MicroPixel requires exactly one configured board")
endif()
