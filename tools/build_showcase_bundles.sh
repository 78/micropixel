#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
apps=(tap-counter color-lab pixel-sketch orbit-pad)

for app_name in "${apps[@]}"; do
    app_dir="$workspace_root/guest/apps/$app_name"
    app_build_dir="$workspace_root/build/apps/$app_name"
    bundle_output="$app_build_dir/$app_name.bundle.bin"
    python3 "$workspace_root/tools/micropixel" package "$app_dir" \
        --profile "${MICROPIXEL_GUEST_PROFILE:-release}" \
        --output-dir "$app_build_dir" \
        --output "$bundle_output"
done
