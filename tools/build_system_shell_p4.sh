#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$workspace_root/firmware/espressif"
build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
system_shell_dir="${P4_SYSTEM_SHELL_OUTPUT_DIR:-$workspace_root/build/system-shell-p4}"
blocks_bundle="${P4_BLOCKS_BUNDLE:-$workspace_root/build/apps/blocks/blocks.bundle.bin}"
snake_bundle="${P4_SNAKE_BUNDLE:-$workspace_root/build/apps/snake/snake.bundle.bin}"
demo_bundle="${P4_DEMO_BUNDLE:-$workspace_root/build/apps/demo/demo.bundle.bin}"
app_store_image="$system_shell_dir/app-store.bin"
sdkconfig_path="${P4_SDKCONFIG:-$build_dir/sdkconfig.release}"
sdkconfig_defaults="${P4_SDKCONFIG_DEFAULTS:-$firmware_dir/sdkconfig.p4.defaults}"

if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
if [[ ! -f "$IDF_PATH/export.sh" ]]; then
    echo "ESP-IDF export script not found: $IDF_PATH/export.sh" >&2
    exit 2
fi

# The flash entry point delegates to this script, so reject known lifecycle,
# gesture, architecture, or App Store regressions before producing hardware
# images. These checks are host-only and complete in a few seconds.
bash "$workspace_root/tools/tests/test_firmware_host.sh"
PYTHONPATH="$workspace_root${PYTHONPATH:+:$PYTHONPATH}" python3 -m unittest tools.tests.test_build_app_store_image
"$workspace_root/tools/check_firmware_architecture.sh"

bash "$workspace_root/tools/build_blocks_bundle.sh"
bash "$workspace_root/tools/build_snake_bundle.sh"
bash "$workspace_root/tools/build_demo_bundle.sh"

source "$IDF_PATH/export.sh" >/dev/null
idf.py -C "$firmware_dir" -B "$build_dir" \
    -D SDKCONFIG="$sdkconfig_path" \
    -D SDKCONFIG_DEFAULTS="$sdkconfig_defaults" \
    -D IDF_TARGET=esp32p4 \
    build

mkdir -p "$system_shell_dir"
python3 "$workspace_root/tools/build_app_store_image.py" \
    --output "$app_store_image" \
    "$blocks_bundle" \
    "$snake_bundle" \
    "$demo_bundle"
bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$app_store_image"

partition_table="$build_dir/partition_table/partition-table.bin"
if [[ ! -f "$partition_table" ]]; then
    echo "Partition table not found after Host build: $partition_table" >&2
    exit 2
fi
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

echo "System Shell P4 image set ready:"
echo "  firmware:  $build_dir/micropixel.bin"
echo "  app store: $app_store_image ($image_size bytes)"
echo "  apps:      $blocks_bundle, $snake_bundle, $demo_bundle"
echo "  target:    $partition_name offset=$partition_offset size=$partition_size"
