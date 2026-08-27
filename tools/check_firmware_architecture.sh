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
fail_on_include platform/common platform/boards \
    "Reusable Platform common code must not depend on a concrete board."
fail_on_include platform/drivers platform/boards \
    "Reusable device drivers must not depend on a concrete board."
fail_on_include platform/lvgl platform/boards \
    "Reusable LVGL profiles must not depend on a concrete board."

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
    "$source_root/platform/metalio-claw4"
    "$source_root/platform/null"
)
for legacy_file in "${legacy_files[@]}"; do
    if [[ -e "$legacy_file" ]]; then
        echo "Legacy Firmware boundary returned: $legacy_file" >&2
        exit 1
    fi
done

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

echo "Firmware architecture check passed."
