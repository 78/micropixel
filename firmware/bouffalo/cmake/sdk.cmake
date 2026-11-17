# SPDX-License-Identifier: Apache-2.0
get_filename_component(MPX_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
execute_process(COMMAND python3 "${MPX_ROOT}/tools/check_sdk.py" --exact
    COMMAND_ERROR_IS_FATAL ANY)
find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

# Public MicroPixel RV32 AOT bundles use the single-float ABI on both chips.
sdk_add_compile_options(-mabi=ilp32f)
sdk_add_link_options(-mabi=ilp32f)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
sdk_add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)

# The SDK adds these prebuilt ILP32D libraries unconditionally on DG B0.
# This foundation uses neither multimedia nor PKA. Drop them rather than
# disabling ABI diagnostics or rewriting vendor objects.
get_property(_libraries GLOBAL PROPERTY SDK_LIBS)
list(FILTER _libraries EXCLUDE REGEX "/(libmultimedia_|libpka_|libpec_v2_)[^/]*[.]a$")
set_property(GLOBAL PROPERTY SDK_LIBS ${_libraries})
