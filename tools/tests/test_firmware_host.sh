#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/../.." && pwd)"
test_output_dir="$workspace_root/build/host-tests"
cxx="${CXX:-/usr/bin/clang++}"
cc="${CC:-/usr/bin/clang}"

if [[ ! -x "$cxx" ]]; then
    echo "C++ compiler not found: $cxx" >&2
    exit 2
fi
if [[ ! -x "$cc" ]]; then
    echo "C compiler not found: $cc" >&2
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

build_and_run_c() {
    local name="$1"
    shift
    local test_binary="$test_output_dir/${name}_test"

    "$cc" \
        -std=c17 \
        -Wall -Wextra -Werror \
        -I "$workspace_root/tools/tests/watchdog_stubs" \
        -I "$workspace_root/firmware/espressif/main" \
        "$@" \
        -o "$test_binary"
    "$test_binary"
}

build_and_run app_controller \
    -pthread \
    -include "$workspace_root/tools/tests/firmware_stubs/runtime/app_runtime.hpp" \
    "$workspace_root/tools/tests/test_app_controller.cpp" \
    "$workspace_root/firmware/espressif/main/host/controller/app_controller.cpp"

build_and_run control_dispatcher \
    -pthread \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_control_dispatcher.cpp" \
    "$workspace_root/firmware/espressif/main/host/controller/control_dispatcher.cpp"

build_and_run background_executor \
    -pthread \
    "$workspace_root/tools/tests/test_background_executor.cpp" \
    "$workspace_root/firmware/espressif/main/work/background_executor.cpp"

build_and_run i2c_executor \
    -pthread \
    "$workspace_root/tools/tests/test_i2c_executor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/buses/i2c_executor.cpp"

build_and_run_c watchdog_timer \
    -pthread \
    "$workspace_root/tools/tests/test_watchdog_timer.c" \
    "$workspace_root/firmware/espressif/main/runtime/wamr/watchdog.c"

build_and_run system_gesture_router \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_system_gesture_router.cpp" \
    "$workspace_root/firmware/espressif/main/host/ui/system_gesture_router.cpp"

build_and_run hall_carousel \
    "$workspace_root/tools/tests/test_hall_carousel.cpp"

build_and_run guest_viewport \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_viewport.cpp"

build_and_run guest_display_transform \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_display_transform.cpp"

build_and_run guest_layout \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_layout.cpp"

build_and_run guest_psram \
    "$workspace_root/tools/tests/test_guest_psram.cpp"

build_and_run guest_button \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_button.cpp"

build_and_run ascii_line_framer \
    "$workspace_root/tools/tests/test_ascii_line_framer.cpp"

build_and_run usb_download_reset_detector \
    "$workspace_root/tools/tests/test_usb_download_reset_detector.cpp"

build_and_run control_curves \
    "$workspace_root/tools/tests/test_control_curves.cpp"

build_and_run synth_mixer \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_synth_mixer.cpp" \
    "$workspace_root/firmware/espressif/main/platform/audio/audio_mixer.cpp"

build_and_run system_shell \
    -pthread \
    "$workspace_root/tools/tests/test_system_shell.cpp" \
    "$workspace_root/firmware/espressif/main/host/ui/system_shell.cpp"

build_and_run system_locale \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_system_locale.cpp" \
    "$workspace_root/firmware/espressif/main/host/ui/system_locale.cpp"

build_and_run font_registry \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_cbin_loader.cpp"

build_and_run font_cbin_loader \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_font_cbin_loader.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_cbin_loader.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_registry.cpp"

build_and_run host_power_state \
    "$workspace_root/tools/tests/test_host_power_state.cpp"

build_and_run device_services \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_device_services.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp"

build_and_run sensor_service \
    -pthread \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_sensor_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/sensor_service.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp"

build_and_run gpio_service \
    -pthread \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_gpio_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/gpio_service.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp"

"$cxx" \
    -std=c++23 \
    -Wall -Wextra -Werror \
    -pthread \
    -I "$workspace_root/firmware/espressif/main" \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/firmware_stubs" \
    "$workspace_root/tools/tests/test_event_queue.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/event_queue.cpp" \
    -o "$test_output_dir/event_queue_test"
"$test_output_dir/event_queue_test"

build_and_run device_catalog \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_device_catalog.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_registry.cpp"

build_and_run app_store \
    "$workspace_root/tools/tests/test_app_store.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundle/app_store.cpp"

build_and_run bundlefs \
    "$workspace_root/tools/tests/test_bundlefs.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundlefs/bundlefs.cpp"

build_and_run bundlefs_16k_mmu \
    -DSPI_FLASH_MMU_PAGE_SIZE=16384U \
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
    "$workspace_root/firmware/espressif/main/host/time/system_time.cpp"

metadata_output_dir="$test_output_dir/package-metadata"
mkdir -p "$metadata_output_dir"
python3 - "$metadata_output_dir" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
(root / "tiny.aot").write_bytes(b"MicroPixel metadata reader test payload.\n")
(root / "app.json").write_text(json.dumps({
    "schema_version": 1,
    "app_id": "micropixel.metadata-test",
    "display_name": {
        "default": "en",
        "values": {
            "en": "Metadata Test",
            "zh-Hans": "元数据测试",
            "zh": "中文测试",
        },
    },
    "display": "square",
    "source": "unused.cpp",
}, ensure_ascii=False), encoding="utf-8")
PY
python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$metadata_output_dir/tiny.aot" \
    --app-manifest "$metadata_output_dir/app.json" \
    --output "$metadata_output_dir/localized.bundle.bin"
MICROPIXEL_TEST_LOCALE=zh-CN \
MICROPIXEL_EXPECT_DISPLAY_NAME=元数据测试 \
MICROPIXEL_EXPECT_METADATA_SCHEMA=1 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=app \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/localized.bundle.bin"
MICROPIXEL_TEST_LOCALE=fr-FR \
MICROPIXEL_EXPECT_DISPLAY_NAME="Metadata Test" \
MICROPIXEL_EXPECT_METADATA_SCHEMA=1 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=app \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/localized.bundle.bin"
python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$metadata_output_dir/tiny.aot" \
    --app-manifest "$metadata_output_dir/app.json" \
    --legacy-metadata-v1 \
    --output "$metadata_output_dir/legacy.bundle.bin"
MICROPIXEL_TEST_LOCALE=zh-CN \
MICROPIXEL_EXPECT_DISPLAY_NAME="Metadata Test" \
MICROPIXEL_EXPECT_METADATA_SCHEMA=0 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=app \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/legacy.bundle.bin"

component_output_dir="$test_output_dir/font-component"
mkdir -p "$component_output_dir"
python3 - "$component_output_dir" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
(root / "raw.cbin").write_bytes(b"component-font-payload")
(root / "charset.txt").write_text("U+0020..U+00FF\n", encoding="utf-8")
(root / "component.json").write_text(json.dumps({
    "schema_version": 1,
    "package_type": "component",
    "component_type": "font",
    "id": "fonts.fixture",
    "display_name": {"default": "en", "values": {"en": "Fixture Fonts"}},
    "version": "1.0.0",
    "languages": ["zh-CN", "zh-Hans"],
    "font_bundle": "fixture-font-v1",
    "charset": "fixture-common-v1",
    "fonts": {
        role: {"asset": f"font.{role}", "style": "regular", "size": size, "bpp": 4}
        for role, size in (("small", 14), ("medium", 18), ("large", 24), ("title", 32))
    },
}), encoding="utf-8")
(root / "assets.json").write_text(json.dumps({
    "schema_version": 1,
    "assets": [
        {"name": f"font.{role}", "path": f"font-{size}.mpxcbin", "format": "font_cbin"}
        for role, size in (("small", 14), ("medium", 18), ("large", 24), ("title", 32))
    ],
}), encoding="utf-8")
PY
for size in 14 18 24 32; do
    python3 "$workspace_root/tools/build_font_cbin.py" \
        --raw-cbin "$component_output_dir/raw.cbin" \
        --profile "fixture-${size}-v1" \
        --size "$size" \
        --charset-source "$component_output_dir/charset.txt" \
        --output "$component_output_dir/font-${size}.mpxcbin"
done
python3 "$workspace_root/tools/build_app_bundle.py" \
    --app-manifest "$component_output_dir/component.json" \
    --asset-manifest "$component_output_dir/assets.json" \
    --prepare-resource-pack "$component_output_dir/resources.pack" \
    --emit-cpp-header "$component_output_dir/resources.hpp" \
    --cpp-namespace fixture_component
python3 "$workspace_root/tools/build_app_bundle.py" \
    --app-manifest "$component_output_dir/component.json" \
    --resource-pack "$component_output_dir/resources.pack" \
    --output "$component_output_dir/fonts.bundle.bin"
MICROPIXEL_EXPECT_DISPLAY_NAME="Fixture Fonts" \
MICROPIXEL_EXPECT_METADATA_SCHEMA=1 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=component \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$component_output_dir/fonts.bundle.bin"
