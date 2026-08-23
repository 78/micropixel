#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash tools/flash_system_shell_p4.sh PORT" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$workspace_root/firmware/espressif"
build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
system_shell_dir="${P4_SYSTEM_SHELL_OUTPUT_DIR:-$workspace_root/build/system-shell-p4}"
app_store_image="$system_shell_dir/app-store.bin"
serial_port="$1"
sdkconfig_path="${P4_SDKCONFIG:-$build_dir/sdkconfig.release}"
sdkconfig_defaults="${P4_SDKCONFIG_DEFAULTS:-$firmware_dir/sdkconfig.p4.defaults}"

if [[ ! -e "$serial_port" ]]; then
    echo "Serial port not found: $serial_port" >&2
    exit 2
fi
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    echo "ESP-IDF export script not found: $IDF_PATH/export.sh" >&2
    exit 2
fi

bash "$workspace_root/tools/build_system_shell_p4.sh"
source "$IDF_PATH/export.sh" >/dev/null

idf.py -C "$firmware_dir" -B "$build_dir" \
    -D SDKCONFIG="$sdkconfig_path" \
    -D SDKCONFIG_DEFAULTS="$sdkconfig_defaults" \
    -D IDF_TARGET=esp32p4 \
    -p "$serial_port" \
    flash

partition_table="$build_dir/partition_table/partition-table.bin"
partition_name="${P4_GUEST_PARTITION:-app_store}"
read -r partition_offset partition_size < <(
    python "$IDF_PATH/components/partition_table/parttool.py" \
        --partition-table-file "$partition_table" \
        get_partition_info --partition-name "$partition_name" --info offset size
)
image_size="$(wc -c < "$app_store_image" | tr -d ' ')"
if (( image_size > partition_size )); then
    echo "App Store image is larger than $partition_name ($image_size > $partition_size)." >&2
    exit 2
fi

python -m esptool --chip esp32p4 --port "$serial_port" --baud 460800 \
    write-flash "$partition_offset" "$app_store_image"
python -m esptool --chip esp32p4 --port "$serial_port" --baud 460800 \
    verify-flash "$partition_offset" "$app_store_image"

echo "System Shell P4 flashed and verified on $serial_port with Blocks, Snake and Demo."
echo "Reboot should stop in App Hall; tap any card to launch its Guest."
