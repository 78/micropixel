#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
profile="reterminal-d1001"

host_build_dir_override="${D1001_HOST_BUILD_DIR:-}"
sdkconfig_override="${D1001_SDKCONFIG:-}"
sdkconfig_defaults_override="${D1001_SDKCONFIG_DEFAULTS:-}"
port_override="${D1001_PORT:-}"
baud_override="${D1001_BAUD:-}"
guests_output_dir_override="${D1001_GUESTS_OUTPUT_DIR:-}"

if [[ -f "$workspace_root/.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$workspace_root/.env"
    set +a
fi

# Explicit caller values win over the repository .env for both this script and
# tools/firmware.py, which reads the same D1001_* variables from the profile.
if [[ -n "$host_build_dir_override" ]]; then export D1001_HOST_BUILD_DIR="$host_build_dir_override"; fi
if [[ -n "$sdkconfig_override" ]]; then export D1001_SDKCONFIG="$sdkconfig_override"; fi
if [[ -n "$sdkconfig_defaults_override" ]]; then export D1001_SDKCONFIG_DEFAULTS="$sdkconfig_defaults_override"; fi
if [[ -n "$port_override" ]]; then export D1001_PORT="$port_override"; fi
if [[ -n "$baud_override" ]]; then export D1001_BAUD="$baud_override"; fi
if [[ -n "$guests_output_dir_override" ]]; then export D1001_GUESTS_OUTPUT_DIR="$guests_output_dir_override"; fi

host_build_dir="${host_build_dir_override:-${D1001_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4-d1001}}"
default_baud="${baud_override:-${D1001_BAUD:-2000000}}"
guests_output_dir="${guests_output_dir_override:-${D1001_GUESTS_OUTPUT_DIR:-$host_build_dir/guests}}"
persisted_port="${port_override:-${D1001_PORT:-}}"
app_store_image="$guests_output_dir/app-store.bin"

usage() {
    cat <<'EOF'
Usage: bash tools/d1001.sh COMMAND [PORT]

Local defaults are loaded from the repository-root .env; explicit environment
variables take precedence.

Commands:
  build-host        Build the Host firmware for Seeed reTerminal D1001
  flash-host [PORT] Flash the existing Host build (app_store preserved)
  monitor [PORT]    Attach to the serial monitor and reset the target
  fullclean-host    Delete the D1001 build directory
  port [PORT]       Identify the chip and MAC on the given or detected port
  build-apps        Build the seven example App Bundles
  flash-apps [PORT] Write the example App Store image into app_store
  flash-all [PORT]  Build Host + Apps, flash the Host, then flash app_store
  flash-empty [PORT] Bring-up: build/flash the Host with an empty App Store
                    catalog (no WASI SDK or WAMRC needed)

Environment:
  D1001_HOST_BUILD_DIR, D1001_SDKCONFIG, D1001_SDKCONFIG_DEFAULTS,
  D1001_PORT, D1001_BAUD, D1001_GUESTS_OUTPUT_DIR,
  MICROPIXEL_GUEST_PROFILE
EOF
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
        echo "IDF_PATH is missing or invalid; set it in the repository-root .env." >&2
        exit 2
    fi
    if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]] &&
        [[ "$(command -v idf.py 2>/dev/null || true)" == "$IDF_PATH/tools/idf.py" ]]; then
        return
    fi
    # shellcheck disable=SC1091
    source "$IDF_PATH/export.sh" >/dev/null
}

d1001() {
    require_idf
    python3 "$workspace_root/tools/firmware.py" "$profile" "$@"
}

build_app_package() {
    local app_name="$1"
    local app_dir="$workspace_root/guest/apps/$app_name"
    local app_build_dir="$workspace_root/build/apps/$app_name"
    python3 "$workspace_root/tools/micropixel" package "$app_dir" \
        --profile "${MICROPIXEL_GUEST_PROFILE:-release}" \
        --aot-target riscv32-ilp32f \
        --output-dir "$app_build_dir" \
        --output "$app_build_dir/$app_name.bundle.bin"
}

build_apps() {
    local app_name
    for app_name in sdk-demo snake maze-evil blocks tilt jump-jump gravity-balls; do
        build_app_package "$app_name"
    done
}

app_store_partition_info() {
    local partition_table="$host_build_dir/partition_table/partition-table.bin"
    if [[ ! -f "$partition_table" ]]; then
        echo "Partition table missing: $partition_table" >&2
        echo "Build the Host first with: bash tools/d1001.sh build-host" >&2
        return 2
    fi
    python "$IDF_PATH/components/partition_table/parttool.py" \
        --partition-table-file "$partition_table" \
        get_partition_info --partition-name app_store --info offset size
}

create_app_store_image() {
    local bundles=(
        "$workspace_root/build/apps/sdk-demo/sdk-demo.bundle.bin"
        "$workspace_root/build/apps/snake/snake.bundle.bin"
        "$workspace_root/build/apps/maze-evil/maze-evil.bundle.bin"
        "$workspace_root/build/apps/blocks/blocks.bundle.bin"
        "$workspace_root/build/apps/tilt/tilt.bundle.bin"
        "$workspace_root/build/apps/jump-jump/jump-jump.bundle.bin"
        "$workspace_root/build/apps/gravity-balls/gravity-balls.bundle.bin"
    )
    local bundle
    for bundle in "${bundles[@]}"; do
        if [[ ! -f "$bundle" ]]; then
            echo "App Bundle missing: $bundle" >&2
            echo "Build them first with: bash tools/d1001.sh build-apps" >&2
            return 2
        fi
    done
    mkdir -p "$guests_output_dir"
    python3 "$workspace_root/tools/build_app_store_image.py" \
        --output "$app_store_image" "${bundles[@]}"
}

# Bring-up path: a valid BundleFS catalog with no Apps. It avoids the
# "catalog scan failed" boot path and needs neither WASI SDK nor WAMRC.
create_empty_app_store_image() {
    local offset size
    read -r offset size < <(app_store_partition_info)
    mkdir -p "$guests_output_dir"
    python3 "$workspace_root/tools/build_app_store_image.py" \
        --output "$app_store_image" --app-store-size "$size"
}

resolve_port() {
    local requested="${1:-$persisted_port}"
    if [[ -z "$requested" ]]; then
        echo "A port is required for App Store flashing; pass PORT or set D1001_PORT." >&2
        return 2
    fi
    printf '%s\n' "$requested"
}

write_app_store_image() {
    local port="$1"
    local offset size image_size
    read -r offset size < <(app_store_partition_info)
    image_size="$(wc -c < "$app_store_image" | tr -d ' ')"
    if ((image_size > size)); then
        echo "App Store image is larger than app_store ($image_size > $size)." >&2
        return 2
    fi
    python -m esptool --chip esp32p4 --port "$port" --baud "$default_baud" \
        write-flash "$offset" "$app_store_image"
}

command="${1:-}"
if [[ $# -gt 0 ]]; then
    shift
fi

case "$command" in
    build-host)
        d1001 build
        ;;
    flash-host)
        arguments=(flash-built)
        if [[ -n "${1:-}" ]]; then
            arguments+=(--port "$1")
        fi
        d1001 "${arguments[@]}"
        ;;
    monitor)
        arguments=(monitor --reset)
        if [[ -n "${1:-}" ]]; then
            arguments+=(--port "$1")
        fi
        d1001 "${arguments[@]}"
        ;;
    fullclean-host)
        d1001 fullclean
        ;;
    port)
        arguments=(port)
        if [[ -n "${1:-}" ]]; then
            arguments+=(--port "$1")
        fi
        d1001 "${arguments[@]}"
        ;;
    build-apps)
        require_idf
        build_apps
        ;;
    flash-apps)
        require_idf
        create_app_store_image
        write_app_store_image "$(resolve_port "${1:-}")"
        echo "App Store flashed. Boot with: bash tools/d1001.sh monitor"
        ;;
    flash-all)
        require_idf
        build_apps
        d1001 build
        create_app_store_image
        port="$(resolve_port "${1:-}")"
        d1001 flash-built --port "$port" --baud "$default_baud"
        write_app_store_image "$port"
        echo "reTerminal D1001 Host and App Store flashed on $port."
        ;;
    flash-empty)
        require_idf
        d1001 build
        create_empty_app_store_image
        port="$(resolve_port "${1:-}")"
        d1001 flash-built --port "$port" --baud "$default_baud"
        write_app_store_image "$port"
        echo "reTerminal D1001 Host flashed on $port with an empty App Store catalog."
        ;;
    help | --help | -h | "")
        usage
        [[ -n "$command" ]] || exit 2
        ;;
    *)
        echo "Unknown command: $command" >&2
        usage >&2
        exit 2
        ;;
esac