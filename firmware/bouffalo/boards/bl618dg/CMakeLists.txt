# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
# FreeRTOSConfig.h must be visible while the SDK creates its kernel target.
include_directories("${CMAKE_CURRENT_LIST_DIR}/../../config")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sdk.cmake")
include("${MPX_ROOT}/cmake/runtime.cmake")
project(micropixel_bl618dg)
