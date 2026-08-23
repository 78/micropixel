#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: bash tools/flash_guest_p4.sh PORT APP_BUNDLE [APP_BUNDLE ...]" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
serial_port="$1"
shift
app_bundles=("$@")
build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
partition_table="$build_dir/partition_table/partition-table.bin"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
idf_root="$IDF_PATH"

if [[ ! -e "$serial_port" ]]; then
    echo "Serial port not found: $serial_port" >&2
    exit 2
fi
for app_bundle in "${app_bundles[@]}"; do
    if [[ ! -f "$app_bundle" ]]; then
        echo "App Bundle not found: $app_bundle" >&2
        exit 2
    fi
done
if [[ ! -f "$partition_table" ]]; then
    echo "P4 partition table not found; build the P4 Host first." >&2
    exit 2
fi
if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "ESP-IDF export script not found: $idf_root/export.sh" >&2
    exit 2
fi

source "$idf_root/export.sh" >/dev/null

partition_name="${P4_GUEST_PARTITION:-app_store}"
read -r partition_offset partition_size < <(
    python "$IDF_PATH/components/partition_table/parttool.py" \
        --partition-table-file "$partition_table" \
        get_partition_info --partition-name "$partition_name" --info offset size
)
temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT
app_store_image="$temporary_dir/app-store.bin"
python3 "$workspace_root/tools/build_app_store_image.py" --output "$app_store_image" "${app_bundles[@]}"
image_size="$(wc -c < "$app_store_image" | tr -d ' ')"
if (( image_size > partition_size )); then
    echo "App Store image is larger than $partition_name ($image_size > $partition_size)." >&2
    exit 2
fi

# Installation is an explicit offline operation. Resolve the exact extent from
# the built table and write only the Bundle sectors; the running Host continues
# to see app_store as read-only.
python -m esptool --chip esp32p4 --port "$serial_port" --baud 460800 \
    write-flash "$partition_offset" "$app_store_image"

echo "P4 App Store flashed to $serial_port with ${#app_bundles[@]} Bundle(s)."
