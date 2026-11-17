# SPDX-License-Identifier: Apache-2.0
execute_process(COMMAND python3 "${MPX_ROOT}/tools/prepare_deps.py" --check
    COMMAND_ERROR_IS_FATAL ANY)
sdk_add_include_directories("${MPX_ROOT}/config" "${MPX_ROOT}/common/include")
target_sources(app PRIVATE "${MPX_ROOT}/common/memory.c")
add_subdirectory("${MPX_ROOT}/common/wamr" wamr)
sdk_set_main_file("${MPX_ROOT}/examples/runtime_smoke.c")
