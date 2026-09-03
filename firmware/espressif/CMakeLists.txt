cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(MICROPIXEL_SUPPORTED_TARGETS esp32p4 esp32s31 esp32s3)
if(NOT IDF_TARGET IN_LIST MICROPIXEL_SUPPORTED_TARGETS)
    message(FATAL_ERROR "MicroPixel supports only ESP32-P4, ESP32-S31 and ESP32-S3")
endif()
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
set(PROJECT_VER "0.5.0")
project(micropixel)

# esp-hosted 2.x still starts its SDIO transport from a C constructor. With
# ESP-IDF 6.1 that constructor runs before the P4 sleep-retention modules are
# registered and crashes in sleep_retention.c. Remove only that auto-init
# translation unit; the Wi-Fi backend starts esp-hosted explicitly from
# app_main after IDF startup has completed.
if(IDF_TARGET STREQUAL "esp32p4")
    idf_component_get_property(esp_hosted_lib espressif__esp_hosted COMPONENT_LIB)
    get_target_property(esp_hosted_sources ${esp_hosted_lib} SOURCES)
    list(FILTER esp_hosted_sources EXCLUDE REGEX "port_esp_hosted_host_init\\.c$")
    set_property(TARGET ${esp_hosted_lib} PROPERTY SOURCES "${esp_hosted_sources}")
endif()

# WAMR 2.4.x relies on sys/stat.h and stdio.h being included indirectly.
# ESP-IDF 6.1 no longer provides those transitive declarations, so inject the
# standard headers without modifying the WAMR submodule from this repository.
idf_component_get_property(wamr_lib wasm-micro-runtime COMPONENT_LIB)
target_compile_options(${wamr_lib} PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:-include>
    $<$<COMPILE_LANGUAGE:C,CXX>:${CMAKE_CURRENT_LIST_DIR}/wamr_espidf_compat.h>
    # WAMR intentionally records a local variable's address as an integer-like
    # stack watermark. GCC 15 diagnoses the storage as a dangling pointer at
    # -O2 even though WAMR never dereferences it after the frame returns.
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-error=dangling-pointer>)
