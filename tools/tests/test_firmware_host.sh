#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/../.." && pwd)"
test_output_dir="$workspace_root/build/host-tests"
cxx="${CXX:-/usr/bin/clang++}"

if [[ ! -x "$cxx" ]]; then
    echo "C++ compiler not found: $cxx" >&2
    exit 2
fi

mkdir -p "$test_output_dir"

build_and_run() {
    local name="$1"
    shift
    local test_binary="$test_output_dir/${name}_test"

    "$cxx" \
        -std=c++23 \
        -Wall -Wextra -Werror \
        -I "$workspace_root/tools/tests/firmware_stubs" \
        -I "$workspace_root/firmware/espressif/main" \
        "$@" \
        -o "$test_binary"
    "$test_binary"
}

build_and_run app_controller \
    -pthread \
    -include "$workspace_root/tools/tests/firmware_stubs/runtime/app_runtime.hpp" \
    "$workspace_root/tools/tests/test_app_controller.cpp" \
    "$workspace_root/firmware/espressif/main/app_controller.cpp"

build_and_run system_gesture_router \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_system_gesture_router.cpp" \
    "$workspace_root/firmware/espressif/main/host_ui/system_gesture_router.cpp"

build_and_run perceptual_control \
    "$workspace_root/tools/tests/test_perceptual_control.cpp"

build_and_run system_shell \
    -pthread \
    "$workspace_root/tools/tests/test_system_shell.cpp" \
    "$workspace_root/firmware/espressif/main/host_ui/system_shell.cpp"
