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

http3_component_dir="$workspace_root/firmware/espressif/components/78__esp-http3"
if [[ ! -f "$http3_component_dir/src/tls/tls_handshake.cc" ]]; then
    http3_component_dir="$workspace_root/firmware/espressif/managed_components/78__esp-http3"
fi
if [[ ! -f "$http3_component_dir/src/tls/tls_handshake.cc" ]]; then
    echo "esp-http3 component not found; build the Host before running Host tests" >&2
    exit 2
fi

mkdir -p "$test_output_dir"

build_and_run() {
    local name="$1"
    shift
    local test_binary="$test_output_dir/${name}_test"

    python3 "$workspace_root/tools/tests/build_host_test.py" "$cxx" \
        -std=c++23 \
        -Wall -Wextra -Werror \
        -I "$workspace_root/tools/tests/firmware_stubs" \
        -I "$workspace_root/firmware/espressif/main" \
        -I "$workspace_root/guest" \
        "$@" \
        -o "$test_binary"
    "$test_binary"
}

build_and_run_c() {
    local name="$1"
    shift
    local test_binary="$test_output_dir/${name}_test"

    python3 "$workspace_root/tools/tests/build_host_test.py" "$cc" \
        -std=c17 \
        -Wall -Wextra -Werror \
        -I "$workspace_root/tools/tests/watchdog_stubs" \
        -I "$workspace_root/firmware/espressif/main" \
        "$@" \
        -o "$test_binary"
    "$test_binary"
}

python3 - "$workspace_root/tools/tests/fixtures/app-requirements-v1.json" "$test_output_dir/app_requirements_fixture.h" <<'PYFIXTURE'
import json, sys
from pathlib import Path
fixture=json.loads(Path(sys.argv[1]).read_text())
lines=[]
for key in ('capabilities','services'):
    lines.append('static const char* const fixture_'+key+'[] = {'+','.join(json.dumps(v) for v in fixture[key])+'};')
lines.append('static const struct { const char* current; const char* candidate; bool update; } fixture_versions[] = {'+','.join('{'+json.dumps(a)+','+json.dumps(b)+','+str(c).lower()+'}' for a,b,c in fixture['versions'])+'};')
Path(sys.argv[2]).write_text('\n'.join(lines)+'\n')
PYFIXTURE
kv_test_defines=()
while IFS= read -r definition; do
    kv_test_defines+=("-D${definition}")
done < <(sed -n '/^CONFIG_MICROPIXEL_KV_/p' "$workspace_root/firmware/espressif/sdkconfig.defaults")
build_and_run private_storage \
    "${kv_test_defines[@]}" \
    "$workspace_root/tools/tests/test_private_storage.cpp" \
    "$workspace_root/tools/tests/fake_nvs.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/storage_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/app_storage.cpp" \
    "$workspace_root/firmware/espressif/main/platform/storage/network_settings_migration.cpp"

build_and_run guest_failure_detail "$workspace_root/tools/tests/test_guest_failure_detail.cpp"
build_and_run linear_memory_policy "$workspace_root/tools/tests/test_linear_memory_policy.cpp"

build_and_run_c app_requirements -I "$test_output_dir" "$workspace_root/tools/tests/test_app_requirements.c"

build_and_run maze_touch_controls \
    "$workspace_root/tools/tests/test_maze_touch_controls.cpp"

build_and_run ft6336_report \
    -I "$workspace_root/tools/tests/touch_stubs" \
    "$workspace_root/tools/tests/test_ft6336_report.cpp" \
    "$workspace_root/firmware/espressif/main/platform/input/ft6336_report.cpp"

build_and_run gravity_balls "$workspace_root/guest/apps/gravity-balls/src/physics_test.cpp"

build_and_run jump_jump \
    "$workspace_root/tools/tests/test_jump_jump.cpp" \
    "$workspace_root/guest/apps/jump-jump/model.cpp" \
    "$workspace_root/guest/apps/jump-jump/renderer.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/graphics/raster_kernels.cpp"

build_and_run frame_timing \
    "$workspace_root/tools/tests/test_frame_timing.cpp"

python3 "$workspace_root/tools/generate_localization.py" \
    --catalog-dir "$workspace_root/firmware/espressif/main/host/ui/i18n" \
    --default-locale en --cpp-namespace host_strings \
    --output-header "$test_output_dir/host_strings.hpp" \
    --report "$test_output_dir/host-localization-report.json"

build_and_run network_controller -pthread \
    "$workspace_root/tools/tests/test_network_controller.cpp" \
    "$workspace_root/firmware/espressif/main/host/network/network_controller.cpp"

build_and_run async_wifi -pthread \
    -iquote "$workspace_root/tools/tests/cellular_stubs" \
    "$workspace_root/tools/tests/test_async_wifi.cpp" \
    "$workspace_root/firmware/espressif/main/host/network/async_wifi.cpp"

build_and_run cellular_controller \
    -I "$test_output_dir" \
    -pthread \
    -iquote "$workspace_root/tools/tests/cellular_stubs" \
    -include "$workspace_root/tools/tests/cellular_stubs/cellular_nvs_declarations.hpp" \
    "$workspace_root/tools/tests/test_cellular_controller.cpp" \
    "$workspace_root/firmware/espressif/main/platform/boards/metalio-claw4/cellular_controller.cpp"

build_and_run guest_timers \
    "$workspace_root/tools/tests/test_guest_timers.cpp" \
    "$workspace_root/guest/runtime/timers.cpp"

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

build_and_run remote_control_policy \
    "$workspace_root/tools/tests/test_remote_control_policy.cpp"

build_and_run i2c_executor \
    -pthread \
    "$workspace_root/tools/tests/test_i2c_executor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/buses/i2c_executor.cpp"

build_and_run polled_vector_sensor \
    -iquote "$workspace_root/tools/tests/sensor_stubs" \
    "$workspace_root/tools/tests/test_polled_vector_sensor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/sensors/polled_vector_sensor_peripheral.cpp" \
    "$workspace_root/firmware/espressif/main/platform/sensors/polled_inertial_sensor_peripheral.cpp"

build_and_run board_gpio \
    -pthread \
    -iquote "$workspace_root/tools/tests/gpio_stubs" \
    "$workspace_root/tools/tests/test_board_gpio.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/gpio/esp_gpio_peripheral.cpp" \
    "$workspace_root/firmware/espressif/main/platform/gpio/gpio_isr_service.cpp"

build_and_run internal_ram \
    -DMICROPIXEL_TEST_INTERNAL_RAM \
    "$workspace_root/tools/tests/test_internal_ram.cpp"

build_and_run_c watchdog_timer \
    -pthread \
    "$workspace_root/tools/tests/test_watchdog_timer.c" \
    "$workspace_root/firmware/espressif/main/runtime/wamr/watchdog.c"

build_and_run system_gesture_router \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_system_gesture_router.cpp" \
    "$workspace_root/firmware/espressif/main/host/ui/system_gesture_router.cpp"

build_and_run host_pointer_event_queue \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_host_pointer_event_queue.cpp"

build_and_run hall_ui \
    "$workspace_root/tools/tests/test_hall_ui.cpp" \
    "$workspace_root/firmware/espressif/main/host/ui/lvgl/square_common/hall_cover_mask.cpp"

build_and_run hall_cover_cache \
    -iquote "$workspace_root/tools/tests/hall_cover_stubs" \
    "$workspace_root/tools/tests/test_hall_cover_cache.cpp" \
    "$workspace_root/firmware/espressif/main/host/ui/lvgl/square_common/hall_cover_cache.cpp"

build_and_run guest_display_configuration \
    "$workspace_root/tools/tests/test_guest_display_configuration.cpp"

build_and_run guest_display_transform \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_display_transform.cpp"

build_and_run snake_gamekit \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_snake_gamekit.cpp"

# Header-only SDK game helpers shared by the Guest apps.
build_and_run sdk_helpers \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_sdk_helpers.cpp"

build_and_run sdk_gamepad \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_sdk_gamepad.cpp"

build_and_run mesh_renderer \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_mesh_renderer.cpp" \
    "$workspace_root/guest/runtime/mesh_renderer.cpp"

build_and_run tomb_room_world \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_tomb_room_world.cpp" \
    "$workspace_root/guest/runtime/mesh_renderer.cpp" \
    "$workspace_root/guest/apps/tomb-explorer/world/level_data.cpp" \
    "$workspace_root/guest/apps/tomb-explorer/world/room_world.cpp" \
    "$workspace_root/guest/apps/tomb-explorer/game/player.cpp" \
    "$workspace_root/guest/apps/tomb-explorer/game/character.cpp"

build_and_run bitmap_store \
    -DMICROPIXEL_TEST_TRACK_HEAP -fsanitize=address,undefined -g \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_bitmap_store.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/resources/bitmap_store.cpp"

# Exercise the pinned decoder with real PNG input and fault-injected PSRAM.
# libpng supports C++ compilation; no system libpng installation is required.
png_component="$workspace_root/firmware/espressif/managed_components/espressif__libpng"
png_sources=()
for source in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans \
    pngwio pngwrite pngwtran pngwutil; do
    png_sources+=("$png_component/libpng/$source.c")
done
build_and_run bitmap_decoder \
    -DMICROPIXEL_TEST_TRACK_HEAP -DPNG_ARM_NEON_OPT=0 -DPNG_INTEL_SSE_OPT=0 \
    -Wno-unused-command-line-argument \
    -fsanitize=address,undefined -g -O2 \
    -I "$png_component" -I "$png_component/libpng" \
    -I "$workspace_root/firmware/espressif/managed_components/espressif__esp_new_jpeg/include" \
    "$workspace_root/tools/tests/test_bitmap_decoder.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/resources/bitmap_decoder.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundle/bundle_section_reader.cpp" \
    -x c++ "${png_sources[@]}" -lz

build_and_run spi_nand_block_storage \
    -fsanitize=address,undefined -g -O2 \
    -I "$workspace_root/tools/tests/nand_stubs" \
    "$workspace_root/tools/tests/test_spi_nand_block_storage.cpp" \
    "$workspace_root/firmware/espressif/main/platform/storage/spi_nand_block_storage.cpp"

build_and_run pixel_compositor \
    "$workspace_root/tools/tests/test_pixel_compositor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/pixel_compositor.cpp"

build_and_run psram_allocator \
    -DMICROPIXEL_TEST_TRACK_HEAP \
    "$workspace_root/tools/tests/test_psram_allocator.cpp"

build_and_run app_surface_compositor \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_app_surface_compositor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/app_surface_compositor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/pixel_compositor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/guest_scene.cpp" \
    "$workspace_root/firmware/espressif/main/device/text.cpp"

build_and_run scene_storage \
    -DMICROPIXEL_TEST_TRACK_PSRAM \
    "$workspace_root/tools/tests/test_scene_storage.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/app_surface_compositor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/pixel_compositor.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/guest_scene.cpp" \
    "$workspace_root/firmware/espressif/main/device/text.cpp"

build_and_run guest_scene \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_scene.cpp" \
    "$workspace_root/firmware/espressif/main/platform/graphics/guest_scene.cpp" \
    "$workspace_root/firmware/espressif/main/device/text.cpp"

build_and_run guest_scene_lifecycle \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_guest_scene_lifecycle.cpp"

build_and_run guest_ui \
    "$workspace_root/tools/tests/test_guest_ui.cpp" \
    -I "$workspace_root/guest"

build_and_run serial_transport \
    "$workspace_root/tools/tests/test_serial_transport.cpp"

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
    "$workspace_root/tools/tests/test_system_locale.cpp" \
    -I "$workspace_root/guest" \
    "$workspace_root/firmware/espressif/main/host/ui/system_locale.cpp" \
    "$workspace_root/firmware/espressif/main/host/time/system_time.cpp"

build_and_run glyph_bitmap \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_glyph_bitmap.cpp"

build_and_run font_fallback \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_font_fallback.cpp"

build_and_run tiny_ttf_font_cache \
    -DMICROPIXEL_TEST_TRACK_PSRAM=1 \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_tiny_ttf_font_cache.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/tiny_ttf_font_cache.cpp"

build_and_run language_packs \
    "$workspace_root/tools/tests/test_language_packs.cpp" \
    "$workspace_root/firmware/espressif/main/host/fonts/language_packs.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundle/app_store.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/app_storage.cpp" \
    "$workspace_root/tools/tests/fake_nvs.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/bundlefs/bundle_store_source.cpp" \
    -x c++ "$workspace_root/firmware/espressif/main/runtime/bundle/memory_bundle_source.c"

build_and_run bounded_ttf_font \
    -DMICROPIXEL_TEST_TRACK_HEAP \
    -DMICROPIXEL_TEST_FONT=\"${MICROPIXEL_TEST_LANGUAGE_FONT:-$workspace_root/firmware/espressif/main/platform/lvgl/fonts/vendor/Montserrat-Medium.ttf}\" \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    -I "$workspace_root/firmware/espressif/managed_components/lvgl__lvgl" \
    "$workspace_root/tools/tests/test_bounded_ttf_font.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/bounded_ttf_font.cpp"

build_and_run font_registry \
    "$workspace_root/tools/tests/test_font_registry.cpp" \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_cbin_loader.cpp"

build_and_run font_registry_box3 \
    -DCONFIG_MICROPIXEL_BOARD_ESP32_S3_BOX_3=1 \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_cbin_loader.cpp"

build_and_run font_registry_mosaico \
    -DCONFIG_MICROPIXEL_BOARD_ESP_MOSAICO=1 \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/font_cbin_stubs" \
    "$workspace_root/tools/tests/test_font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_registry.cpp" \
    "$workspace_root/firmware/espressif/main/platform/lvgl/fonts/font_cbin_loader.cpp"

build_and_run power_policy \
    "$workspace_root/tools/tests/test_power_policy.cpp"

build_and_run device_services \
    -I "$workspace_root/guest" \
    "$workspace_root/tools/tests/test_device_services.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp"

build_and_run peripheral_lifecycle \
    "$workspace_root/tools/tests/test_peripheral_lifecycle.cpp" \
    -pthread \
    -I "$workspace_root/guest" \
    "$workspace_root/firmware/espressif/main/runtime/services/sensor_service.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/gpio_service.cpp"

# Uses the real EventQueue (FreeRTOS shims), so the firmware include path must
# precede the stub directory like event_queue_test below.
python3 "$workspace_root/tools/tests/build_host_test.py" "$cxx" \
    -std=c++23 \
    -Wall -Wextra -Werror \
    -pthread \
    -I "$workspace_root/firmware/espressif/main" \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/firmware_stubs" \
    "$workspace_root/tools/tests/test_direct_surface_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/direct_surface_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/event_queue.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp" \
    -o "$test_output_dir/direct_surface_service_test"
"$test_output_dir/direct_surface_service_test"

# Graphics 1.6 raster kernels and the RasterService in front of them; shares
# the in-flight veto with DirectSurfaceService, hence the same link set.
# Exercise the telemetry branch too, including its otherwise-unused counters.
python3 "$workspace_root/tools/tests/build_host_test.py" "$cxx" \
    -std=c++23 \
    -Wall -Wextra -Werror \
    -pthread \
    -I "$workspace_root/firmware/espressif/main" \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/firmware_stubs" \
    -DCONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG=1 \
    -DMICROPIXEL_TEST_TRACK_HEAP -fsanitize=address,undefined -g \
    "$workspace_root/tools/tests/test_raster_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/graphics/raster_kernels.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/raster_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/services/direct_surface_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/event_queue.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp" \
    -o "$test_output_dir/raster_service_test"
"$test_output_dir/raster_service_test"

python3 "$workspace_root/tools/tests/build_host_test.py" "$cxx" \
    -std=c++23 \
    -Wall -Wextra -Werror \
    -pthread \
    -I "$workspace_root/firmware/espressif/main" \
    -I "$workspace_root/guest" \
    -I "$workspace_root/tools/tests/firmware_stubs" \
    "$workspace_root/tools/tests/test_pcm_stream_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/audio/pcm_stream_service.cpp" \
    "$workspace_root/firmware/espressif/main/runtime/event_queue.cpp" \
    "$workspace_root/firmware/espressif/main/device/device_services.cpp" \
    -o "$test_output_dir/pcm_stream_service_test"
"$test_output_dir/pcm_stream_service_test"

python3 "$workspace_root/tools/tests/build_host_test.py" "$cxx" \
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

# The memory Bundle source is plain C shared with the Bundle reader; the Host
# test compiles it as C++ so one driver invocation links everything (keep it last).
app_store_sources=(
    "$workspace_root/tools/tests/test_app_store.cpp"
    "$workspace_root/firmware/espressif/main/runtime/bundle/app_store.cpp"
    "$workspace_root/firmware/espressif/main/runtime/services/app_storage.cpp"
    "$workspace_root/tools/tests/fake_nvs.cpp"
    "$workspace_root/firmware/espressif/main/runtime/bundlefs/bundle_store_source.cpp"
    -x c++ "$workspace_root/firmware/espressif/main/runtime/bundle/memory_bundle_source.c"
)

build_and_run app_store -DMICROPIXEL_TEST_TRACK_HEAP "${app_store_sources[@]}"

build_and_run app_store_s3 -DMICROPIXEL_TEST_TRACK_HEAP \
    -DCONFIG_IDF_TARGET_ESP32P4=0 -DCONFIG_IDF_TARGET_ESP32S3=1 \
    "${app_store_sources[@]}"

bundlefs_sources=(
    "$workspace_root/tools/tests/test_bundlefs.cpp"
    "$workspace_root/firmware/espressif/main/runtime/bundlefs/bundlefs.cpp"
    "$workspace_root/firmware/espressif/main/platform/storage/partition_block_storage.cpp"
    "$workspace_root/firmware/espressif/main/platform/storage/flash_page_mapping_cache.cpp"
)

build_and_run flash_page_mapping_cache \
    -pthread -DMICROPIXEL_TEST_TRACK_HEAP \
    "$workspace_root/tools/tests/test_flash_page_mapping_cache.cpp" \
    "$workspace_root/firmware/espressif/main/platform/storage/flash_page_mapping_cache.cpp"

build_and_run bundlefs "${bundlefs_sources[@]}"

build_and_run bundlefs_16k_mmu \
    -DSPI_FLASH_MMU_PAGE_SIZE=16384U \
    "${bundlefs_sources[@]}"

build_and_run http3_tls_parser \
    -I "$http3_component_dir/include" \
    "$workspace_root/tools/tests/test_http3_tls_parser.cpp" \
    "$http3_component_dir/src/tls/tls_handshake.cc" \
    "$http3_component_dir/src/esp_http3_memory.cc"

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
    "title": {
        "default": "en",
        "values": {
            "en": "Metadata Test",
            "zh-Hans": "元数据测试",
            "zh": "中文测试",
        },
    },
    "sources": ["unused.cpp"],
}, ensure_ascii=False), encoding="utf-8")
PY
python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$metadata_output_dir/tiny.aot" \
    --aot-target riscv32-ilp32f \
    --app-manifest "$metadata_output_dir/app.json" \
    --output "$metadata_output_dir/localized.bundle.bin"
MICROPIXEL_TEST_LOCALE=zh-CN \
MICROPIXEL_EXPECT_DISPLAY_NAME=元数据测试 \
MICROPIXEL_EXPECT_METADATA_SCHEMA=1 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=app \
MICROPIXEL_EXPECT_PACKAGE_VERSION= \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/localized.bundle.bin"
MICROPIXEL_TEST_LOCALE=fr-FR \
MICROPIXEL_EXPECT_DISPLAY_NAME="Metadata Test" \
MICROPIXEL_EXPECT_METADATA_SCHEMA=1 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=app \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/localized.bundle.bin"
python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$metadata_output_dir/tiny.aot" \
    --aot-target riscv32-ilp32f \
    --app-manifest "$metadata_output_dir/app.json" \
    --legacy-metadata-v1 \
    --output "$metadata_output_dir/legacy.bundle.bin"
MICROPIXEL_TEST_LOCALE=zh-CN \
MICROPIXEL_EXPECT_DISPLAY_NAME="Metadata Test" \
MICROPIXEL_EXPECT_METADATA_SCHEMA=0 \
MICROPIXEL_EXPECT_PACKAGE_TYPE=app \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/legacy.bundle.bin"

# Generate version fixtures through the real serializer, bypassing manifest
# validation for malformed values so the Host must validate them independently.
python3 - "$workspace_root" "$metadata_output_dir" <<'PY'
import json
import sys
from pathlib import Path

workspace, root = map(Path, sys.argv[1:])
sys.path.insert(0, str(workspace))
from tools import build_app_bundle as builder

serialize = builder.serialize_package_metadata
for name, version in [("versioned", "0.1.0"), ("trailing-dot", "1.2.3."),
                      ("leading-zero", "01.2.3"), ("prerelease", "1.2.3-beta"),
                      ("empty", ""), ("numeric", 123), ("null", None),
                      ("oversized", "1" * 28 + ".0.0")]:
    def serialize_fixture(manifest):
        payload = json.loads(serialize(manifest))
        payload["version"] = version
        return json.dumps(payload, separators=(",", ":")).encode()
    builder.serialize_package_metadata = serialize_fixture
    sys.argv = ["build_app_bundle.py", "--aot", str(root / "tiny.aot"),
                "--aot-target", "riscv32-ilp32f", "--app-manifest", str(root / "app.json"),
                "--output", str(root / f"{name}.bundle.bin")]
    builder.main()
for name, font in [("font-required", "zh-CN"), ("font-unknown", "zh"), ("font-type", ["zh-CN"])]:
    def serialize_fixture(manifest):
        payload = json.loads(serialize(manifest))
        payload["core_abi"] = builder.CORE_ABI_VERSION
        payload["requirements"] = {"schema_version": 1, "display": {"layouts": ["square"], "min_width": 320, "min_height": 320},
                                   "required": [], "optional": [], "any_of": [], "services": {}, "system_font": font}
        return json.dumps(payload, separators=(",", ":")).encode()
    builder.serialize_package_metadata = serialize_fixture
    sys.argv = ["build_app_bundle.py", "--aot", str(root / "tiny.aot"), "--aot-target", "riscv32-ilp32f",
                "--app-manifest", str(root / "app.json"), "--output", str(root / f"{name}.bundle.bin")]
    builder.main()

PY
MICROPIXEL_EXPECT_PACKAGE_VERSION=0.1.0 \
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/versioned.bundle.bin"
MICROPIXEL_EXPECT_SYSTEM_FONT=zh-CN bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/font-required.bundle.bin"
for invalid_version in trailing-dot leading-zero prerelease empty numeric null oversized font-unknown font-type; do
    MICROPIXEL_EXPECT_INVALID_METADATA=1 \
        bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$metadata_output_dir/$invalid_version.bundle.bin"
done

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
    "title": {"default": "en", "values": {"en": "Fixture Fonts"}},
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

# A static TTF component contains one font and no executable AOT.
python3 - "$component_output_dir" "$workspace_root" <<'PYTTF'
import json, shutil, sys
from pathlib import Path
root, workspace = map(Path, sys.argv[1:])
manifest = json.loads((root / 'component.json').read_text())
manifest.pop('fonts')
manifest['font'] = {'asset': 'regular', 'format': 'ttf'}
(root / 'ttf.json').write_text(json.dumps(manifest))
shutil.copyfile(workspace / 'firmware/espressif/main/platform/lvgl/fonts/vendor/Montserrat-Medium.ttf', root / 'regular.ttf')
(root / 'ttf-assets.json').write_text(json.dumps({'schema_version': 1, 'assets': [{'name': 'regular', 'format': 'font_ttf', 'path': 'regular.ttf'}]}))
PYTTF
python3 "$workspace_root/tools/build_app_bundle.py" --app-manifest "$component_output_dir/ttf.json" \
    --asset-manifest "$component_output_dir/ttf-assets.json" --prepare-resource-pack "$component_output_dir/ttf.pack" \
    --emit-cpp-header "$component_output_dir/ttf.hpp" --cpp-namespace fixture_ttf
python3 "$workspace_root/tools/build_app_bundle.py" --app-manifest "$component_output_dir/ttf.json" \
    --resource-pack "$component_output_dir/ttf.pack" --output "$component_output_dir/ttf.bundle.bin"
MICROPIXEL_EXPECT_PACKAGE_TYPE=component bash "$workspace_root/tools/tests/test_bundle_reader.sh" "$component_output_dir/ttf.bundle.bin"

# Blocks timing and difficulty are pure Guest model logic.
build_and_run blocks_model \
    -DMICROPIXEL_MODEL_TESTING \
    "$workspace_root/guest/apps/blocks/blocks_model.cpp" \
    "$workspace_root/guest/apps/blocks/blocks_model_test.cpp"

# Exercise real LVGL flex layout and scrolling, including the compact 320x240 Hall.
cmake -S "$workspace_root/tools/tests/lvgl_ui" -B "$test_output_dir/lvgl-ui" \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE=Release
cmake --build "$test_output_dir/lvgl-ui" --target hall_error_dialog_test system_menu_test cellular_ui_test wifi_ui_test --parallel 4
(cd "$test_output_dir/lvgl-ui" && ./hall_error_dialog_test)

(cd "$test_output_dir/lvgl-ui" && ./system_menu_test)

(cd "$test_output_dir/lvgl-ui" && ./cellular_ui_test)

(cd "$test_output_dir/lvgl-ui" && ./wifi_ui_test)

cmake -S "$workspace_root/tools/tests/network_json" -B "$test_output_dir/network-json"
cmake --build "$test_output_dir/network-json" --parallel 2
"$test_output_dir/network-json/network_json_test"
