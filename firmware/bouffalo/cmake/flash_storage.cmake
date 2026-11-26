# SPDX-License-Identifier: Apache-2.0
get_filename_component(MPX_REPO "${MPX_ROOT}/../.." ABSOLUTE)
target_include_directories(app PRIVATE "${MPX_REPO}/firmware/espressif/main")
target_sources(app PRIVATE
    "${MPX_ROOT}/common/storage.cpp"
    "${MPX_ROOT}/common/app_storage.cpp")
