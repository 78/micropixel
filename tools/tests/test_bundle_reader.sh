#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: bash tools/tests/test_bundle_reader.sh APP_BUNDLE [APP_BUNDLE ...]" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/../.." && pwd)"
test_output_dir="$workspace_root/build/host-tests"
test_binary="$test_output_dir/bundle_reader_test"
cc="${CC:-/usr/bin/clang}"

if [[ ! -x "$cc" ]]; then
    echo "C compiler not found: $cc" >&2
    exit 2
fi
for app_bundle in "$@"; do
    if [[ ! -f "$app_bundle" ]]; then
        echo "App Bundle not found: $app_bundle" >&2
        exit 2
    fi
done

mkdir -p "$test_output_dir"
"$cc" \
    -std=c17 \
    -Wall -Wextra -Werror \
    -I "$workspace_root/tools/tests/firmware_stubs" \
    -I "$workspace_root/firmware/espressif/main" \
    "$workspace_root/tools/tests/test_bundle_reader.c" \
    "$workspace_root/firmware/espressif/main/runtime/bundle/bundle_reader.c" \
    -o "$test_binary"

for app_bundle in "$@"; do
    "$test_binary" "$app_bundle"
done
