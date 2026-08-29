set(micropixel_sources
    "app_main.cpp"
    "firmware_app.cpp"
)
set(micropixel_builtin_font_output_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/fonts")
set(micropixel_builtin_font_sources
    "${micropixel_builtin_font_output_dir}/font_builtin_latin_14.c"
    "${micropixel_builtin_font_output_dir}/font_builtin_latin_18.c"
    "${micropixel_builtin_font_output_dir}/font_builtin_latin_24.c"
    "${micropixel_builtin_font_output_dir}/font_builtin_latin_32.c"
)
list(APPEND micropixel_sources ${micropixel_builtin_font_sources})
# ESP-IDF includes component CMake files once in script mode while discovering
# requirements. Source properties are only available during the real configure
# pass; the custom-command outputs are registered as generated there.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_source_files_properties(${micropixel_builtin_font_sources} PROPERTIES GENERATED TRUE)
endif()
set(micropixel_requires 78__esp-http3 app_update cjson esp_app_format esp_netif esp_wifi mbedtls nvs_flash pthread)

include("${CMAKE_CURRENT_LIST_DIR}/host/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/work/CMakeLists.txt")
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

# LVGL's ESP-IDF Kconfig choice can only select one of LVGL's bundled fonts as
# LV_FONT_DEFAULT. Make the generated builtin-latin-v1 small font the actual
# framework default as well, so Montserrat 14 is not retained as a second Latin
# glyph set. The custom declaration is visible to LVGL and all of its users.
idf_component_get_property(micropixel_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(
    ${micropixel_lvgl_lib} PUBLIC
    "LV_FONT_MONTSERRAT_14=0"
    "LV_FONT_CUSTOM_DECLARE=LV_FONT_DECLARE(font_builtin_latin_14)"
    "LV_FONT_DEFAULT=&font_builtin_latin_14"
)
if(CONFIG_MICROPIXEL_LVGL_OBJECT_POOL_PSRAM)
    # LVGL's builtin allocator normally backs its TLSF pool with an untagged
    # static array in internal .bss. LVGL 9.5 applies this attribute only to
    # that pool, so ESP-IDF can map it into external-RAM BSS without moving
    # LVGL's mutex or allocator state out of internal memory.
    target_compile_definitions(${micropixel_lvgl_lib} PRIVATE
        "LV_ATTRIBUTE_LARGE_RAM_ARRAY=__attribute__((section(\".ext_ram.bss\")))")
endif()

idf_build_get_property(micropixel_python PYTHON)
set(micropixel_host_i18n_dir "${CMAKE_CURRENT_LIST_DIR}/host/ui/i18n")
file(GLOB micropixel_host_i18n_catalogs CONFIGURE_DEPENDS "${micropixel_host_i18n_dir}/*.json")
set(micropixel_host_i18n_output_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(micropixel_host_i18n_header "${micropixel_host_i18n_output_dir}/host_strings.hpp")
set(micropixel_host_i18n_report "${micropixel_host_i18n_output_dir}/host-localization-report.json")
add_custom_command(
    OUTPUT "${micropixel_host_i18n_header}" "${micropixel_host_i18n_report}"
    COMMAND "${micropixel_python}" "${CMAKE_CURRENT_LIST_DIR}/../../../tools/generate_localization.py"
            --catalog-dir "${micropixel_host_i18n_dir}"
            --default-locale en
            --cpp-namespace host_strings
            --output-header "${micropixel_host_i18n_header}"
            --report "${micropixel_host_i18n_report}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../../tools/generate_localization.py" ${micropixel_host_i18n_catalogs}
    VERBATIM
)
add_custom_target(micropixel_host_i18n DEPENDS "${micropixel_host_i18n_header}" "${micropixel_host_i18n_report}")
add_dependencies(${COMPONENT_LIB} micropixel_host_i18n)
target_include_directories(${COMPONENT_LIB} PRIVATE "${micropixel_host_i18n_output_dir}")

set(micropixel_builtin_font_profile
    "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl/fonts/builtin-latin-v1.json")
set(micropixel_lvgl_font_source_dir
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl/scripts/built_in_font")
set(micropixel_lvgl_symbol_def
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl/src/font/lv_symbol_def.h")
set(micropixel_lvgl_keyboard_source
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl/src/widgets/keyboard/lv_keyboard.c")
set(micropixel_lvgl_dropdown_source
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl/src/widgets/dropdown/lv_dropdown.c")
file(GLOB_RECURSE micropixel_lvgl_symbol_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/host/ui/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/host/ui/*.cc"
    "${CMAKE_CURRENT_LIST_DIR}/host/ui/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/host/ui/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/host/ui/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl/*.cc"
    "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl/*.h"
    "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl/*.hpp")
set(micropixel_builtin_font_report
    "${micropixel_builtin_font_output_dir}/builtin-latin-v1-report.json")
add_custom_command(
    OUTPUT ${micropixel_builtin_font_sources} "${micropixel_builtin_font_report}"
    COMMAND "${micropixel_python}" "${CMAKE_CURRENT_LIST_DIR}/../../../tools/generate_builtin_fonts.py"
            --profile "${micropixel_builtin_font_profile}"
            --host-catalog-report "${micropixel_host_i18n_report}"
            --montserrat "${micropixel_lvgl_font_source_dir}/Montserrat-Medium.ttf"
            --replacement-font "${micropixel_lvgl_font_source_dir}/DejaVuSans.ttf"
            --symbol-font "${micropixel_lvgl_font_source_dir}/FontAwesome5-Solid+Brands+Regular.woff"
            --lvgl-symbol-def "${micropixel_lvgl_symbol_def}"
            --lvgl-symbol-source "${CMAKE_CURRENT_LIST_DIR}/host/ui"
            --lvgl-symbol-source "${CMAKE_CURRENT_LIST_DIR}/platform/lvgl"
            --lvgl-symbol-source "${micropixel_lvgl_keyboard_source}"
            --lvgl-symbol-source "${micropixel_lvgl_dropdown_source}"
            --output-dir "${micropixel_builtin_font_output_dir}"
            --report "${micropixel_builtin_font_report}"
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../../tools/generate_builtin_fonts.py"
            "${micropixel_builtin_font_profile}"
            "${micropixel_host_i18n_report}"
            "${micropixel_lvgl_font_source_dir}/Montserrat-Medium.ttf"
            "${micropixel_lvgl_font_source_dir}/DejaVuSans.ttf"
            "${micropixel_lvgl_font_source_dir}/FontAwesome5-Solid+Brands+Regular.woff"
            "${micropixel_lvgl_symbol_def}"
            "${micropixel_lvgl_keyboard_source}"
            "${micropixel_lvgl_dropdown_source}"
            ${micropixel_lvgl_symbol_sources}
    VERBATIM
)
add_custom_target(micropixel_builtin_fonts DEPENDS ${micropixel_builtin_font_sources}
                                                        "${micropixel_builtin_font_report}")
add_dependencies(${COMPONENT_LIB} micropixel_builtin_fonts)

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
