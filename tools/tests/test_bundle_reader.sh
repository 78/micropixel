#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash tools/tests/test_bundle_reader.sh APP_STORE_IMAGE" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/../.." && pwd)"
test_output_dir="$workspace_root/build/host-tests"
test_binary="$test_output_dir/bundle_reader_test"
app_store_image="$1"
cc="${CC:-/usr/bin/clang}"

if [[ ! -x "$cc" ]]; then
    echo "C compiler not found: $cc" >&2
    exit 2
fi
if [[ ! -f "$app_store_image" ]]; then
    echo "App Store image not found: $app_store_image" >&2
    exit 2
fi

mkdir -p "$test_output_dir"
"$cc" \
    -std=c17 \
    -Wall -Wextra -Werror \
    -I "$workspace_root/tools/tests/firmware_stubs" \
    -I "$workspace_root/firmware/espressif/main" \
    "$workspace_root/tools/tests/test_bundle_reader.c" \
    "$workspace_root/firmware/espressif/main/runtime/bundle/bundle_reader.c" \
    -o "$test_binary"

"$test_binary" "$app_store_image"
