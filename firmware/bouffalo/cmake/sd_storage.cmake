# SPDX-License-Identifier: Apache-2.0
sdk_add_compile_definitions(-DCONFIG_SDH_BUS_WIDTH_1LINE)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(app PRIVATE
    "${MPX_ROOT}/common/sd_storage.c"
    "${MPX_ROOT}/common/sd_app_storage.cpp")
