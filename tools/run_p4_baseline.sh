#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash tools/run_p4_baseline.sh PORT" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
serial_port="$1"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
idf_root="$IDF_PATH"
hello_bundle="$workspace_root/build/bundles/hello.bundle.bin"

bash "$workspace_root/tools/build_p4_baseline.sh"
python3 "$workspace_root/tools/build_app_bundle.py" \
    --aot "$workspace_root/build/guest-p4/sdk_hello.aot" \
    --app-id micropixel.hello \
    --output "$hello_bundle"
P4_APP_BUNDLE="$hello_bundle" \
    bash "$workspace_root/tools/flash_p4_baseline.sh" "$serial_port"
if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "ESP-IDF export script not found: $idf_root/export.sh" >&2
    exit 2
fi
source "$idf_root/export.sh" >/dev/null
python "$workspace_root/tools/capture_serial_until.py" \
    "$serial_port" \
    "Guest main returned successfully" \
    --timeout 20 \
    --reset

echo "P4 baseline passed: SDK Hello Guest loaded and returned successfully."
