#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash tools/monitor_p4.sh PORT" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$workspace_root/firmware/espressif"
build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
idf_root="$IDF_PATH"
serial_port="$1"
sdkconfig_path="${P4_SDKCONFIG:-$build_dir/sdkconfig.release}"

if [[ ! -e "$serial_port" ]]; then
    echo "Serial port not found: $serial_port" >&2
    exit 2
fi
if [[ ! -f "$build_dir/micropixel.elf" ]]; then
    echo "P4 firmware ELF not found; run tools/build_p4_baseline.sh first." >&2
    exit 2
fi
if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "ESP-IDF export script not found: $idf_root/export.sh" >&2
    exit 2
fi

source "$idf_root/export.sh" >/dev/null
exec idf.py -C "$firmware_dir" -B "$build_dir" \
    -D SDKCONFIG="$sdkconfig_path" \
    -D SDKCONFIG_DEFAULTS="$firmware_dir/sdkconfig.p4.defaults" \
    -D IDF_TARGET=esp32p4 \
    -p "$serial_port" monitor
