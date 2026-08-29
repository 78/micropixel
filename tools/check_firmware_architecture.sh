#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
source_root="$workspace_root/firmware/espressif/main"

fail_on_include() {
    local directory="$1"
    local forbidden_prefix="$2"
    local explanation="$3"
    local matches
    matches="$(rg -n "^[[:space:]]*#include[[:space:]]+\"${forbidden_prefix}/" "$source_root/$directory" || true)"
    if [[ -n "$matches" ]]; then
        printf '%s\n' "$matches" >&2
        printf '%s\n' "$explanation" >&2
        exit 1
    fi
}

fail_on_include runtime platform "Runtime must depend on Device contracts, not Platform implementations."
fail_on_include device platform "Device contracts and façades must not depend on Platform implementations."
fail_on_include platform runtime "Platform implementations must not depend on Runtime session types."
fail_on_include host/ui host/controller \
    "Host UI is a presentation boundary and must not depend on Host Controller orchestration."
fail_on_include platform/adapters platform/boards \
    "Reusable Platform adapters must not depend on a concrete board."
fail_on_include platform/buses platform/boards \
    "Reusable Platform bus coordinators must not depend on a concrete board."
fail_on_include platform/audio platform/boards \
    "Reusable Platform audio components must not depend on a concrete board."
fail_on_include platform/controllers platform/boards \
    "Reusable Platform controllers must not depend on a concrete board."
fail_on_include platform/defaults platform/boards \
    "Reusable Platform defaults must not depend on a concrete board."
fail_on_include platform/drivers platform/boards \
    "Reusable device drivers must not depend on a concrete board."
fail_on_include platform/haptics platform/boards \
    "Reusable Platform haptics components must not depend on a concrete board."
fail_on_include platform/lvgl platform/boards \
    "Reusable LVGL profiles must not depend on a concrete board."
fail_on_include platform/random platform/boards \
    "Reusable Platform random sources must not depend on a concrete board."
fail_on_include platform/transports platform/boards \
    "Reusable transports must not depend on a concrete board."
fail_on_include platform/wifi platform/boards \
    "Reusable Platform Wi-Fi components must not depend on a concrete board."

host_lvgl_platform_namespaces="$(rg -n \
    'namespace[[:space:]]+micropixel::platform::lvgl' \
    "$source_root/host/ui/lvgl" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$host_lvgl_platform_namespaces" ]]; then
    printf '%s\n' "$host_lvgl_platform_namespaces" >&2
    echo "Host LVGL implementation must use the micropixel::host_ui::lvgl namespace." >&2
    exit 1
fi

board_system_ui_backends="$(find "$source_root/platform/boards" -type f \
    \( -name 'system_ui_backend.cpp' -o -name 'system_ui_backend.hpp' \) -print)"
if [[ -n "$board_system_ui_backends" ]]; then
    printf '%s\n' "$board_system_ui_backends" >&2
    echo "Board System UI integration belongs in typed presentation.* interfaces, not a board backend." >&2
    exit 1
fi

board_product_ui_includes="$(rg -n \
    '^[[:space:]]*#include[[:space:]]+"host/ui/lvgl/square_common/(app_management_ui|appearance_ui|guest_gesture_hint_ui|hall_card_ui|hall_carousel|hall_catalog|hall_cover_cache|hall_scene_ui|power_management_ui|remote_control_ui|status_layer_ui|system_detail_ui|system_menu_ui|virtualized_hall_policy|wifi_settings_ui)\.hpp"' \
    "$source_root/platform/boards" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$board_product_ui_includes" ]]; then
    printf '%s\n' "$board_product_ui_includes" >&2
    echo "Board code must compose the shared System UI boundary, not include Host UI widgets or Hall policies." >&2
    exit 1
fi

board_system_ui_lifecycle="$(rg -n \
    '\b(ShowHallImpl|UpdateHallStatusBarImpl|UpdateHallInstallProgressImpl|PauseHallCoverLoadingImpl|LeaveHallImpl|DrawHallCard|HallCardEvent|HallCarouselEvent|ShowStartingScreen)\b|hall_cover_cache\.BindUi|hall_scene_ui\.DrawLocked' \
    "$source_root/platform/boards" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$board_system_ui_lifecycle" ]]; then
    printf '%s\n' "$board_system_ui_lifecycle" >&2
    echo "Startup, Hall widgets, cache binding and product UI lifecycle belong in host/ui/lvgl, not board code." >&2
    exit 1
fi

board_system_ui_state_access="$(rg -n \
    '\.ui\.(root|host_pointer|input_router|hall_|status_layer_ui|guest_gesture_hint_ui|launch_image_descriptor|DeleteRootLocked|ResetHallPresentationLocked|ShowLaunchBitmap|DismissLaunchBitmap)' \
    "$source_root/platform/boards" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$board_system_ui_state_access" ]]; then
    printf '%s\n' "$board_system_ui_state_access" >&2
    echo "Board code must use SquareSystemUiState lifecycle hooks instead of reaching into Host UI state." >&2
    exit 1
fi

board_system_ui_profile_assembly="$(rg -n \
    '\bSquareSystemUiProfile[[:space:]]+[A-Za-z_][A-Za-z0-9_]*|\.(hall_card|hall_scene|system_menu|system_page)[[:space:]]*=' \
    "$source_root/platform/boards" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$board_system_ui_profile_assembly" ]]; then
    printf '%s\n' "$board_system_ui_profile_assembly" >&2
    echo "Boards must select a complete 480/720 System UI profile instead of assembling product UI properties." >&2
    exit 1
fi

board_owned_device_identity="$(rg -n \
    '\bmicropixel_device_id_t\b|\bMICROPIXEL_DEVICE_(KIND|CAP)_[A-Z0-9_]+' \
    "$source_root/platform/boards" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$board_owned_device_identity" ]]; then
    printf '%s\n' "$board_owned_device_identity" >&2
    echo "Boards register providers and local endpoints; DeviceRegistry owns public IDs, kinds and capabilities." >&2
    exit 1
fi

board_runtime_device_services="$(rg -n \
    'device::(DeviceCatalog|Sensors|Gpio|Haptics)\b' \
    "$source_root/platform/boards" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$board_runtime_device_services" ]]; then
    printf '%s\n' "$board_runtime_device_services" >&2
    echo "Boards implement local Peripherals and omit unsupported capabilities; Platform owns Guest-facing Device services." >&2
    exit 1
fi

legacy_role_names="$(rg -n \
    '\b(class|struct|using)[[:space:]]+[A-Za-z0-9_]*(Backend|Provider)\b' \
    "$source_root/device" "$source_root/platform" "$source_root/host" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$legacy_role_names" ]]; then
    printf '%s\n' "$legacy_role_names" >&2
    echo "Firmware roles must use Board, Peripheral, Controller, Presentation, Device, Service, Endpoint or Adapter consistently." >&2
    exit 1
fi

board_catalog_files="$(find "$source_root/platform/boards" -type f \
    \( -name 'device_catalog.cpp' -o -name 'device_catalog.hpp' -o -name 'peripheral_ids.hpp' \) -print)"
if [[ -n "$board_catalog_files" ]]; then
    printf '%s\n' "$board_catalog_files" >&2
    echo "Board-owned catalogs and public peripheral ID tables are forbidden." >&2
    exit 1
fi

legacy_callback_interfaces="$(rg -n \
    '\b(SystemUiOperations|SquareSystemUiHardwareHooks)\b|square_system_ui_hardware_hooks\.hpp' \
    "$source_root" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$legacy_callback_interfaces" ]]; then
    printf '%s\n' "$legacy_callback_interfaces" >&2
    echo "Optional System UI hardware capabilities must use typed interfaces, not callback tables." >&2
    exit 1
fi

legacy_files=(
    "$source_root/host_bridge.h"
    "$source_root/bundle_format.h"
    "$source_root/runtime/abi_adapter.cpp"
    "$source_root/runtime/native_api.c"
    "$source_root/runtime/service_handler.hpp"
    "$source_root/runtime/engine.cpp"
    "$source_root/runtime/engine.hpp"
    "$source_root/platform/graphics_backend.hpp"
    "$source_root/platform/random_backend.hpp"
    "$source_root/device/contracts/hardware_info.hpp"
    "$source_root/platform/adapters/system_ui_adapter.cpp"
    "$source_root/platform/adapters/system_ui_adapter.hpp"
    "$source_root/host/ui/lvgl/square_common/square_system_ui_backend.cpp"
    "$source_root/host/ui/lvgl/square_common/square_system_ui_backend.hpp"
    "$source_root/platform/backends"
    "$source_root/platform/common"
    "$source_root/platform/radio"
    "$source_root/platform/soc"
    "$source_root/platform/drivers/input"
    "$source_root/platform/drivers/CMakeLists.txt"
    "$source_root/platform/boards/metalio-claw4/board_drivers.cpp"
    "$source_root/platform/boards/metalio-claw4/board_drivers.hpp"
    "$source_root/platform/boards/metalio-claw4/perceptual_control.hpp"
    "$source_root/platform/metalio-claw4"
    "$source_root/platform/null"
)
for legacy_file in "${legacy_files[@]}"; do
    if [[ -e "$legacy_file" ]]; then
        echo "Legacy Firmware boundary returned: $legacy_file" >&2
        exit 1
    fi
done

legacy_platform_names="$(rg -n \
    '\bBoardDrivers\b|\bScreenCaptureDevelopment\b|namespace[[:space:]]+micropixel::platform::mosaico\b' \
    "$source_root/platform" --glob '*.{cpp,hpp}' || true)"
if [[ -n "$legacy_platform_names" ]]; then
    printf '%s\n' "$legacy_platform_names" >&2
    echo "Legacy Platform names returned." >&2
    exit 1
fi

if rg -n '\besp_restart[[:space:]]*\(' "$source_root/firmware_app.cpp"; then
    echo "Guest completion must return to the Host System Shell, not restart the device." >&2
    exit 1
fi

abi_file_count="$(find "$source_root/runtime/abi" -maxdepth 1 -type f | wc -l | tr -d ' ')"
if [[ "$abi_file_count" != "7" ]]; then
    echo "runtime/abi must remain a cohesive seven-file boundary; found $abi_file_count files." >&2
    exit 1
fi

if rg -n '\bCONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE\b|^[[:space:]]*config[[:space:]]+MICROPIXEL_LVGL_DIRTY_COALESCE[[:space:]]*$' \
    "$source_root" "$workspace_root/firmware/espressif" --glob '!managed_components/**'; then
    echo "Dirty-region coalescing is product behavior and must not regain an enable/disable switch." >&2
    exit 1
fi

usb_local_control="$source_root/platform/transports/usb_serial_jtag_local_control.cpp"
finite_usb_notification_waits="$(rg -n 'ulTaskNotifyTake[[:space:]]*\(' "$usb_local_control" | \
    rg -v 'portMAX_DELAY' || true)"
if [[ -n "$finite_usb_notification_waits" ]]; then
    printf '%s\n' "$finite_usb_notification_waits" >&2
    echo "USB local control must remain event-driven; finite task-notification waits are polling." >&2
    exit 1
fi
if ! rg -q 'ulTaskNotifyTake[[:space:]]*\([^;]*portMAX_DELAY' "$usb_local_control"; then
    echo "USB local control must block on its RX/response-ready event notification." >&2
    exit 1
fi

echo "Firmware architecture check passed."
