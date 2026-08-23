cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "MicroPixel v1 firmware supports only ESP32-P4")
endif()
idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.esp32p4)
project(micropixel)

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
