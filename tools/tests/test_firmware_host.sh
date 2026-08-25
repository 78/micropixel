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

build_and_run app_store \
    "$workspace_root/tools/tests/test_app_store.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundle/app_store.cpp"

build_and_run bundlefs \
    "$workspace_root/tools/tests/test_bundlefs.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundlefs/bundlefs.cpp"

build_and_run http3_tls_parser \
    -I "$workspace_root/firmware/espressif/components/78__esp-http3/include" \
    "$workspace_root/tools/tests/test_http3_tls_parser.cpp" \
    "$workspace_root/firmware/espressif/components/78__esp-http3/src/tls/tls_handshake.cc"

build_and_run remote_control_reconnect_policy \
    "$workspace_root/tools/tests/test_remote_control_reconnect_policy.cpp"

build_and_run system_time \
    "$workspace_root/tools/tests/test_system_time.cpp" \
    "$workspace_root/firmware/espressif/main/system_time.cpp"
