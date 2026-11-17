# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/sdk.cmake")
sdk_set_main_file("${MPX_ROOT}/examples/build_smoke.c")
project(micropixel_bl616cl)
