#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$workspace_root/firmware/espressif"
build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
guest_dir="${P4_GUEST_OUTPUT_DIR:-$workspace_root/build/guest-p4}"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
idf_root="$IDF_PATH"
sdkconfig_path="${P4_SDKCONFIG:-$build_dir/sdkconfig.release}"
sdkconfig_defaults="${P4_SDKCONFIG_DEFAULTS:-$firmware_dir/sdkconfig.p4.defaults}"

if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "ESP-IDF export script not found: $idf_root/export.sh" >&2
    exit 2
fi

source "$idf_root/export.sh" >/dev/null

bash "$workspace_root/tools/check_firmware_style.sh" --format-only
bash "$workspace_root/tools/build_guest_p4.sh"
idf.py -C "$firmware_dir" -B "$build_dir" \
    -D SDKCONFIG="$sdkconfig_path" \
    -D SDKCONFIG_DEFAULTS="$sdkconfig_defaults" \
    -D IDF_TARGET=esp32p4 \
    build

echo "P4 baseline build complete:"
echo "  firmware: $build_dir/micropixel.bin"
echo "  guest AOT: $guest_dir/sdk_hello.aot"
