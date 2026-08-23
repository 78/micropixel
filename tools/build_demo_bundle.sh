#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
app_dir="$workspace_root/guest/apps/demo"
app_build_dir="${P4_DEMO_BUILD_DIR:-$workspace_root/build/apps/demo}"
guest_dir="${P4_GUEST_OUTPUT_DIR:-$app_build_dir}"
asset_dir="${P4_DEMO_ASSET_DIR:-$app_build_dir/assets}"
bundle_output="${P4_BUNDLE_OUTPUT:-$app_build_dir/demo.bundle.bin}"
resource_pack="$asset_dir/resources.pack"

guest_sources=()
while IFS= read -r source_name; do
    guest_sources+=("$app_dir/$source_name")
done < <(
    python3 - "$app_dir/app.json" <<'PY'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
sources = manifest.get("sources")
if not isinstance(sources, list) or not sources or not all(isinstance(source, str) for source in sources):
    raise SystemExit("demo app manifest requires a non-empty string sources list")
if sources[0] != manifest.get("source"):
    raise SystemExit("demo app manifest source must be the first sources entry")
for source in sources:
    print(source)
PY
)

python3 "$workspace_root/tools/build_app_bundle.py" \
    --app-manifest "$app_dir/app.json" \
    --asset-manifest "$app_dir/assets/manifest.json" \
    --prepare-resource-pack "$resource_pack" \
    --emit-cpp-header "$asset_dir/demo_assets.hpp" \
    --cpp-namespace demo_assets

MICROPIXEL_GUEST_PROFILE="${MICROPIXEL_GUEST_PROFILE:-release}" \
MICROPIXEL_GUEST_GENERATED_INCLUDE_DIR="$asset_dir" \
P4_GUEST_OUTPUT_DIR="$guest_dir" \
    bash "$workspace_root/tools/build_guest_app_p4.sh" \
    "${guest_sources[0]}" demo "${guest_sources[@]:1}"

python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$guest_dir/demo.aot" \
    --app-manifest "$app_dir/app.json" \
    --resource-pack "$resource_pack" \
    --output "$bundle_output"

echo "Demo app built: $bundle_output"
ls -lh "$bundle_output"
