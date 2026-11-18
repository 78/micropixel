# SPDX-License-Identifier: Apache-2.0
execute_process(COMMAND python3 "${MPX_ROOT}/tools/prepare_deps.py" --check
    COMMAND_ERROR_IS_FATAL ANY)
sdk_add_include_directories("${MPX_ROOT}/config" "${MPX_ROOT}/common/include")
target_sources(app PRIVATE "${MPX_ROOT}/common/memory.c")
add_subdirectory("${MPX_ROOT}/common/wamr" wamr)
sdk_set_main_file("${MPX_ROOT}/examples/runtime_smoke.c")

# Generate a valid module rather than treating invalid-module rejection as AOT
# execution coverage. Build wamrc from the dependency pin (see docs/runtime.md).
find_program(MPX_SMOKE_WAMRC NAMES wamrc REQUIRED)
find_program(MPX_SMOKE_CLANG NAMES clang-18 REQUIRED)
find_program(MPX_SMOKE_WASM_LD NAMES wasm-ld-18 REQUIRED)
set(MPX_SMOKE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/runtime_smoke_aot.h")
add_custom_command(OUTPUT "${MPX_SMOKE_HEADER}"
    COMMAND python3 "${MPX_ROOT}/tools/build_smoke_aot.py"
        --clang "${MPX_SMOKE_CLANG}" --wasm-ld "${MPX_SMOKE_WASM_LD}" --wamrc "${MPX_SMOKE_WAMRC}"
        --source "${MPX_ROOT}/examples/runtime_guest.c" --output "${MPX_SMOKE_HEADER}"
    DEPENDS "${MPX_ROOT}/examples/runtime_guest.c" "${MPX_ROOT}/tools/build_smoke_aot.py"
        "${MPX_SMOKE_WAMRC}"
    VERBATIM)
target_sources(app PRIVATE "${MPX_SMOKE_HEADER}")
target_include_directories(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
