cmake_minimum_required(VERSION 3.20)
project(micropixel_lvgl_ui_tests LANGUAGES C CXX)
get_filename_component(workspace_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${workspace_root}/firmware/espressif/managed_components/lvgl__lvgl" lvgl)
add_executable(hall_error_dialog_test
    ../test_hall_error_dialog.cpp
    "${workspace_root}/firmware/espressif/main/host/ui/lvgl/square_common/hall_error_dialog.cpp")
target_include_directories(hall_error_dialog_test PRIVATE "${workspace_root}/firmware/espressif/main")
target_compile_features(hall_error_dialog_test PRIVATE cxx_std_23)
target_compile_options(hall_error_dialog_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(hall_error_dialog_test PRIVATE lvgl)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(catalog_dir "${workspace_root}/firmware/espressif/main/host/ui/i18n")
file(GLOB catalogs CONFIGURE_DEPENDS "${catalog_dir}/*.json")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
    COMMAND "${Python3_EXECUTABLE}" "${workspace_root}/tools/generate_localization.py"
        --catalog-dir "${catalog_dir}" --default-locale en --cpp-namespace host_strings
        --output-header "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
        --report "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.json"
    DEPENDS ${catalogs} "${workspace_root}/tools/generate_localization.py")
add_executable(system_menu_test ../test_system_menu.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
    "${workspace_root}/firmware/espressif/main/host/ui/lvgl/square_common/system_menu_ui.cpp")
target_include_directories(system_menu_test PRIVATE stubs ../firmware_stubs
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${workspace_root}/firmware/espressif/main" "${workspace_root}/guest")
target_compile_features(system_menu_test PRIVATE cxx_std_23)
target_compile_options(system_menu_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(system_menu_test PRIVATE lvgl)

add_executable(cellular_ui_test ../test_cellular_ui.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
    "${workspace_root}/firmware/espressif/main/host/ui/lvgl/square_common/status_layer_ui.cpp")
target_include_directories(cellular_ui_test PRIVATE stubs ../firmware_stubs
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${workspace_root}/firmware/espressif/main" "${workspace_root}/guest")
target_compile_features(cellular_ui_test PRIVATE cxx_std_23)
target_compile_options(cellular_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cellular_ui_test PRIVATE lvgl)

add_executable(wifi_ui_test ../test_wifi_ui.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/host_strings.hpp"
    "${workspace_root}/firmware/espressif/main/host/ui/lvgl/square_common/wifi_settings_ui.cpp"
    "${workspace_root}/firmware/espressif/main/host/ui/lvgl/square_common/default_keyboard.cpp")
target_include_directories(wifi_ui_test PRIVATE stubs ../firmware_stubs
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${workspace_root}/firmware/espressif/main" "${workspace_root}/guest")
target_compile_features(wifi_ui_test PRIVATE cxx_std_23)
target_compile_options(wifi_ui_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(wifi_ui_test PRIVATE lvgl)
