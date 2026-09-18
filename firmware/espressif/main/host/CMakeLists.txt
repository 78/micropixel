# Host-owned lifecycle control, system UI, and wall-clock support.
list(APPEND micropixel_sources
    "host/logging/system_log_buffer.cpp"
    "host/fonts/language_packs.cpp"
    "host/network/async_wifi.cpp"
    "host/network/network_controller.cpp"
    "host/network/network_maintenance.cpp"
    "host/time/network_time.cpp"
    "host/time/system_time.cpp"
)

include("${CMAKE_CURRENT_LIST_DIR}/controller/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/ui/CMakeLists.txt")
