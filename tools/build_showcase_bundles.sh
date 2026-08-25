#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
apps=(tap-counter color-lab pixel-sketch orbit-pad)

for app_name in "${apps[@]}"; do
    app_dir="$workspace_root/guest/apps/$app_name"
    app_build_dir="$workspace_root/build/apps/$app_name"
    bundle_output="$app_build_dir/$app_name.bundle.bin"
    mkdir -p "$app_build_dir"

    MICROPIXEL_GUEST_PROFILE="${MICROPIXEL_GUEST_PROFILE:-release}" \
    P4_GUEST_OUTPUT_DIR="$app_build_dir" \
        bash "$workspace_root/tools/build_guest_app_p4.sh" "$app_dir/main.cpp" "$app_name"

    python3 "$workspace_root/tools/build_app_bundle.py" \
        --aot "$app_build_dir/$app_name.aot" \
        --app-manifest "$app_dir/app.json" \
        --output "$bundle_output"

    echo "Showcase App built: $bundle_output"
    ls -lh "$bundle_output"
done
