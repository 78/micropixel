#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
firmware_dir="$workspace_root/firmware/espressif"
source_dir="$firmware_dir/main"
if [[ -z "${IDF_PATH:-}" ]]; then
    echo "IDF_PATH is not set; source ESP-IDF's export.sh first." >&2
    exit 2
fi
idf_root="$IDF_PATH"
product_build_dir="${FIRMWARE_TIDY_BUILD_DIR:-$workspace_root/build/host-p4-firmware-clang}"
conformance_build_dir="${FIRMWARE_CONFORMANCE_TIDY_BUILD_DIR:-$workspace_root/build/host-p4-firmware-clang-conformance}"

run_format=true
run_tidy=true
configure=false

usage() {
    echo "Usage: bash tools/check_firmware_style.sh [--format-only|--tidy-only] [--configure]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --format-only)
            run_tidy=false
            ;;
        --tidy-only)
            run_format=false
            ;;
        --configure)
            configure=true
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
    shift
done

if [[ ! -f "$idf_root/export.sh" ]]; then
    echo "ESP-IDF export script not found: $idf_root/export.sh" >&2
    exit 2
fi

source "$idf_root/export.sh" >/dev/null 2>&1

if $configure; then
    IDF_TOOLCHAIN=clang idf.py -C "$firmware_dir" -B "$product_build_dir" \
        -D SDKCONFIG="$product_build_dir/sdkconfig.style" \
        -D SDKCONFIG_DEFAULTS="$firmware_dir/sdkconfig.p4.defaults" \
        -D IDF_TARGET=esp32p4 reconfigure
    IDF_TOOLCHAIN=clang idf.py -C "$firmware_dir" -B "$conformance_build_dir" \
        -D SDKCONFIG="$conformance_build_dir/sdkconfig.style" \
        -D SDKCONFIG_DEFAULTS="$firmware_dir/sdkconfig.p4.defaults;$firmware_dir/sdkconfig.p4-conformance.defaults" \
        -D IDF_TARGET=esp32p4 reconfigure
fi

source_files=()
while IFS= read -r source_file; do
    source_files+=("$source_file")
done < <(find "$source_dir" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) | sort)

if $run_format; then
    clang-format --dry-run --Werror "${source_files[@]}"
    if rg -n '^[[:space:]]*using[[:space:]]+namespace[[:space:]]+' "$source_dir" \
        --glob '*.c' --glob '*.h' --glob '*.cpp' --glob '*.hpp'; then
        echo "Firmware source must not use a using-directive at namespace scope." >&2
        exit 1
    fi
    bash "$workspace_root/tools/check_firmware_architecture.sh"
    echo "Firmware format check passed (${#source_files[@]} files)."
fi

if ! $run_tidy; then
    exit 0
fi

product_database="$product_build_dir/compile_commands.json"
conformance_database="$conformance_build_dir/compile_commands.json"
if [[ ! -f "$product_database" || ! -f "$conformance_database" ]]; then
    echo "Clang compilation databases are missing." >&2
    echo "Generate them with: bash tools/check_firmware_style.sh --configure" >&2
    exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to assemble the firmware clang-tidy database." >&2
    exit 2
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT
combined_database="$temporary_dir/compile_commands.json"

jq -s '.[0] + .[1] | unique_by(.file)' \
    "$product_database" "$conformance_database" > "$combined_database"

cpp_sources=()
while IFS= read -r source_file; do
    cpp_sources+=("$source_file")
done < <(jq -r --arg prefix "$source_dir/" \
    '.[] | .file | select(startswith($prefix)) | select(endswith(".cpp"))' "$combined_database" | sort -u)

clang-tidy -p "$temporary_dir" --config-file "$workspace_root/firmware/.clang-tidy" "${cpp_sources[@]}"
echo "Firmware clang-tidy check passed (${#cpp_sources[@]} translation units)."
