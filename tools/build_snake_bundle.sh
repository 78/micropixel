#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
app_dir="$workspace_root/guest/apps/snake"
app_build_dir="${P4_SNAKE_BUILD_DIR:-$workspace_root/build/apps/snake}"
guest_dir="${P4_GUEST_OUTPUT_DIR:-$app_build_dir}"
bundle_output="${P4_BUNDLE_OUTPUT:-$app_build_dir/snake.bundle.bin}"

exec python3 "$workspace_root/tools/micropixel" package "$app_dir" \
    --profile "${MICROPIXEL_GUEST_PROFILE:-release}" \
    --output-dir "$guest_dir" \
    --output "$bundle_output"
