#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash tools/flash_p4_baseline.sh PORT" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$workspace_root/firmware/espressif"
build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
app_bundle="${P4_APP_BUNDLE:-$workspace_root/build/apps/snake/snake.bundle.bin}"
partition_table="$build_dir/partition_table/partition-table.bin"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
idf_root="$IDF_PATH"
serial_port="$1"
sdkconfig_path="${P4_SDKCONFIG:-$build_dir/sdkconfig.release}"
sdkconfig_defaults="${P4_SDKCONFIG_DEFAULTS:-$firmware_dir/sdkconfig.p4.defaults}"

if [[ ! -e "$serial_port" ]]; then
    echo "Serial port not found: $serial_port" >&2
    exit 2
fi
if [[ ! -f "$build_dir/micropixel.bin" ]]; then
    echo "P4 firmware not found; run tools/build_p4_baseline.sh first." >&2
    exit 2
fi
if [[ ! -f "$app_bundle" ]]; then
    echo "App Bundle not found: $app_bundle" >&2
    exit 2
fi
if [[ ! -f "$partition_table" ]]; then
    echo "Partition table not found: $partition_table" >&2
    exit 2
fi
if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "ESP-IDF export script not found: $idf_root/export.sh" >&2
    exit 2
fi

source "$idf_root/export.sh" >/dev/null

idf.py -C "$firmware_dir" -B "$build_dir" \
    -D SDKCONFIG="$sdkconfig_path" \
    -D SDKCONFIG_DEFAULTS="$sdkconfig_defaults" \
    -D IDF_TARGET=esp32p4 \
    -p "$serial_port" flash

partition_name="${P4_GUEST_PARTITION:-app_store}"
read -r partition_offset partition_size < <(
    python "$IDF_PATH/components/partition_table/parttool.py" \
        --partition-table-file "$partition_table" \
        get_partition_info --partition-name "$partition_name" --info offset size
)
bundle_size="$(wc -c < "$app_bundle" | tr -d ' ')"
if (( bundle_size > partition_size )); then
    echo "App Bundle is larger than $partition_name ($bundle_size > $partition_size)." >&2
    exit 2
fi
python -m esptool --chip esp32p4 --port "$serial_port" --baud 460800 \
    write-flash "$partition_offset" "$app_bundle"

echo "P4 firmware and App Bundle flashed to $serial_port"
