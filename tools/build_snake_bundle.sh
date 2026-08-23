#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
app_dir="$workspace_root/guest/apps/snake"
app_build_dir="${P4_SNAKE_BUILD_DIR:-$workspace_root/build/apps/snake}"
guest_dir="${P4_GUEST_OUTPUT_DIR:-$app_build_dir}"
asset_dir="${P4_SNAKE_ASSET_DIR:-$app_build_dir/assets}"
bundle_output="${P4_BUNDLE_OUTPUT:-$app_build_dir/snake.bundle.bin}"
resource_pack="$asset_dir/resources.pack"
sfx_manifest="$app_dir/audio/sfx.json"
sfx_device_profile="${MICROPIXEL_SFX_DEVICE_PROFILE:-$workspace_root/firmware/espressif/main/platform/metalio-claw4/audio/perceptual_profile.json}"
sfx_header="$asset_dir/snake_sfx_profiles.hpp"
sfx_report="$app_build_dir/sfx-report.json"

python3 "$workspace_root/tools/analyze_sfx.py" \
    --manifest "$sfx_manifest" \
    --device-profile "$sfx_device_profile" \
    --emit-cpp-header "$sfx_header" \
    --report "$sfx_report" \
    --check

python3 "$workspace_root/tools/build_app_bundle.py" \
    --app-manifest "$app_dir/app.json" \
    --asset-manifest "$app_dir/assets/manifest.json" \
    --prepare-resource-pack "$resource_pack" \
    --emit-cpp-header "$asset_dir/snake_assets.hpp" \
    --cpp-namespace snake_assets

MICROPIXEL_GUEST_PROFILE="${MICROPIXEL_GUEST_PROFILE:-release}" \
MICROPIXEL_GUEST_GENERATED_INCLUDE_DIR="$asset_dir" \
P4_GUEST_OUTPUT_DIR="$guest_dir" \
    bash "$workspace_root/tools/build_guest_app_p4.sh" \
    "$app_dir/main.cpp" snake \
    "$app_dir/snake_app.cpp" \
    "$app_dir/snake_model.cpp" \
    "$app_dir/snake_game.cpp" \
    "$app_dir/snake_renderer.cpp" \
    "$app_dir/snake_effects.cpp" \
    "$app_dir/snake_audio.cpp"
python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$guest_dir/snake.aot" \
    --app-manifest "$app_dir/app.json" \
    --resource-pack "$resource_pack" \
    --output "$bundle_output"

echo "Snake app built: $bundle_output"
ls -lh "$bundle_output"
