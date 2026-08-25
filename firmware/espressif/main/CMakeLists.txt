set(micropixel_sources
    "app_controller.cpp"
    "app_main.cpp"
    "firmware_app.cpp"
    "host_controller.cpp"
    "host_ui/system_gesture_router.cpp"
    "host_ui/system_settings_store.cpp"
    "host_ui/system_shell.cpp"
    "network_time.cpp"
    "remote_control/control_protocol.cpp"
    "remote_control/remote_control_agent.cpp"
    "system_time.cpp"
)
set(micropixel_requires 78__esp-http3 app_update cjson esp_app_format esp_netif esp_wifi mbedtls nvs_flash pthread)

include("${CMAKE_CURRENT_LIST_DIR}/device/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/runtime/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/platform/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/conformance/CMakeLists.txt")

idf_component_register(
    SRCS ${micropixel_sources}
    INCLUDE_DIRS "."
                 "${CMAKE_CURRENT_LIST_DIR}/../../../guest"
    REQUIRES ${micropixel_requires}
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
# ESP-IDF 6.1 defaults globally to the newest compiler mode (currently
# gnu++26), which is appended after compile-feature flags. Keep this Host layer
# on the Framework's explicit C++23 profile.
target_compile_options(
    ${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)
