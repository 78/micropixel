#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
idf_path_override="${IDF_PATH:-}"
s3_port_override="${S3_PORT:-}"
s3_baud_override="${S3_BAUD:-}"
szpi_port_override="${SZPI_S3_PORT:-}"
szpi_baud_override="${SZPI_S3_BAUD:-}"
cores3_port_override="${CORES3_S3_PORT:-}"
cores3_baud_override="${CORES3_S3_BAUD:-}"
s3_host_build_dir_override="${S3_HOST_BUILD_DIR:-}"
szpi_host_build_dir_override="${SZPI_S3_HOST_BUILD_DIR:-}"
cores3_host_build_dir_override="${CORES3_S3_HOST_BUILD_DIR:-}"
s3_apps_output_dir_override="${S3_APPS_OUTPUT_DIR:-}"
xtensa_wamrc_override="${XTENSA_WAMRC:-}"
remote_control_host_override="${MICROPIXEL_REMOTE_CONTROL_HOST:-}"
remote_control_port_override="${MICROPIXEL_REMOTE_CONTROL_PORT:-}"
remote_control_tls_override="${MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS:-}"
remote_control_ca_override="${MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64:-}"
if [[ -f "$workspace_root/.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$workspace_root/.env"
    set +a
fi
if [[ -n "$idf_path_override" ]]; then
    IDF_PATH="$idf_path_override"
fi
if [[ -n "$s3_port_override" ]]; then
    S3_PORT="$s3_port_override"
fi
if [[ -n "$s3_baud_override" ]]; then
    S3_BAUD="$s3_baud_override"
fi
if [[ -n "$szpi_port_override" ]]; then
    SZPI_S3_PORT="$szpi_port_override"
fi
if [[ -n "$szpi_baud_override" ]]; then
    SZPI_S3_BAUD="$szpi_baud_override"
fi
if [[ -n "$cores3_port_override" ]]; then
    CORES3_S3_PORT="$cores3_port_override"
fi
if [[ -n "$cores3_baud_override" ]]; then
    CORES3_S3_BAUD="$cores3_baud_override"
fi
if [[ -n "$s3_host_build_dir_override" ]]; then
    S3_HOST_BUILD_DIR="$s3_host_build_dir_override"
fi
if [[ -n "$szpi_host_build_dir_override" ]]; then
    SZPI_S3_HOST_BUILD_DIR="$szpi_host_build_dir_override"
fi
if [[ -n "$cores3_host_build_dir_override" ]]; then
    CORES3_S3_HOST_BUILD_DIR="$cores3_host_build_dir_override"
fi
if [[ -n "$s3_apps_output_dir_override" ]]; then
    S3_APPS_OUTPUT_DIR="$s3_apps_output_dir_override"
fi
if [[ -n "$xtensa_wamrc_override" ]]; then
    XTENSA_WAMRC="$xtensa_wamrc_override"
fi
if [[ -n "$remote_control_host_override" ]]; then
    MICROPIXEL_REMOTE_CONTROL_HOST="$remote_control_host_override"
fi
if [[ -n "$remote_control_port_override" ]]; then
    MICROPIXEL_REMOTE_CONTROL_PORT="$remote_control_port_override"
fi
if [[ -n "$remote_control_tls_override" ]]; then
    MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS="$remote_control_tls_override"
fi
if [[ -n "$remote_control_ca_override" ]]; then
    MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64="$remote_control_ca_override"
fi

firmware_dir="$workspace_root/firmware/espressif"
common_max_task_name_len="$(sed -n 's/^CONFIG_FREERTOS_MAX_TASK_NAME_LEN=//p' "$firmware_dir/sdkconfig.defaults")"
apps_output_dir="${S3_APPS_OUTPUT_DIR:-$workspace_root/build/esp32s3-apps}"
apps_store="$apps_output_dir/app-store.bin"
host_build_dir="${S3_HOST_BUILD_DIR:-$workspace_root/build/host-esp32s3-box-3}"
szpi_host_build_dir="${SZPI_S3_HOST_BUILD_DIR:-$workspace_root/build/host-esp32s3-szpi}"
cores3_host_build_dir="${CORES3_S3_HOST_BUILD_DIR:-$workspace_root/build/host-esp32s3-cores3}"
xtensa_wamrc="${XTENSA_WAMRC:-$workspace_root/build/tools/wamrc-xtensa/wamrc}"

usage() {
    cat <<'EOF'
Usage: bash tools/s3.sh COMMAND [BOARD] [PORT] [--reset]

Boards: box3 (default), szpi, cores3

Common ESP32-S3 preview commands:
  build-null          Compile the ESP32-S3 hardware-independent Null gate.
  build-host [BOARD]  Build one board Host.
  build-wamrc         Build locked WAMR 2.4.3 wamrc with the Xtensa LLVM backend.
  build-apps          Build Demo, Blocks, Snake and Tilt Xtensa AOT Bundles plus app_store.
  build-release [BOARD]
                      Build one Host and release Apps, then create its
                      browser-flashable micropixel-full.bin image.
  flash-host [BOARD] [PORT]
                      Flash the already-built Host, preserving app_store.
  flash-apps [BOARD] [PORT]
                      Flash only the shared ESP32-S3 app_store image.
  flash-all [BOARD] [PORT]
                      Build and flash one Host plus the shared App Store.
  monitor [BOARD] [PORT] [--reset]
                      Monitor one Host; optionally reset to capture boot.
  port [BOARD] [PORT] Resolve and verify one board serial port.

Compatibility aliases:
  build-szpi          Alias for build-host szpi.
  flash-szpi [PORT]   Alias for flash-host szpi [PORT].
  monitor-szpi [PORT] [--reset]
  port-szpi [PORT]
  build-cores3        Alias for build-host cores3.
  flash-cores3 [PORT] Alias for flash-host cores3 [PORT].
  monitor-cores3 [PORT] [--reset]
  port-cores3 [PORT]

S3_PORT/S3_BAUD, SZPI_S3_PORT/SZPI_S3_BAUD, CORES3_S3_PORT/CORES3_S3_BAUD
and MICROPIXEL_REMOTE_CONTROL_* may be set in the repository-root .env.
Explicit environment variables and a command-line PORT take precedence.
Each preview profile selects its own panel, touch, codec, power and sensor
wiring while reusing the ESP32-S3 Runtime and Xtensa Guest baseline.
EOF
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
        echo "IDF_PATH is missing or invalid; set it in the repository-root .env." >&2
        exit 2
    fi
    if [[ "${CONDA_SHLVL:-0}" != 0 ]]; then
        if [[ -z "${CONDA_EXE:-}" || ! -x "$CONDA_EXE" ]]; then
            echo "Conda is active, but CONDA_EXE is unavailable; run 'conda deactivate' first." >&2
            exit 2
        fi
        eval "$("$CONDA_EXE" shell.bash hook 2>/dev/null)"
        while (( ${CONDA_SHLVL:-0} > 0 )); do
            conda deactivate
        done
    fi
    if [[ "$(command -v idf.py 2>/dev/null || true)" != "$IDF_PATH/tools/idf.py" ]]; then
        # shellcheck disable=SC1090
        source "$IDF_PATH/export.sh" >/dev/null
    fi
}

profile_action() {
    local profile="$1"
    local action="$2"
    shift 2
    python "$workspace_root/tools/firmware.py" "$profile" "$action" "$@"
}

build_profile() {
    local profile="$1"
    local build_dir="$2"
    shift 2
    local partial_buffer_height=""
    local default_path
    for default_path in "$@"; do
        local configured_height
        configured_height="$(sed -n 's/^CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT=//p' \
            "$firmware_dir/$default_path")"
        if [[ -n "$configured_height" ]]; then
            partial_buffer_height="$configured_height"
        fi
    done
    local remote_host="${MICROPIXEL_REMOTE_CONTROL_HOST:-}"
    local remote_port="${MICROPIXEL_REMOTE_CONTROL_PORT:-8443}"
    local allow_unverified="${MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS:-y}"
    local trusted_ca="${MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64:-}"
    if [[ -n "$remote_host" && ! "$remote_host" =~ ^[A-Za-z0-9._:-]+$ ]]; then
        echo "MICROPIXEL_REMOTE_CONTROL_HOST contains unsupported characters: $remote_host" >&2
        exit 2
    fi
    if [[ ! "$remote_port" =~ ^[0-9]+$ ]] || (( remote_port < 1 || remote_port > 65535 )); then
        echo "MICROPIXEL_REMOTE_CONTROL_PORT must be between 1 and 65535: $remote_port" >&2
        exit 2
    fi
    case "$allow_unverified" in
        y | Y | yes | YES | true | TRUE | 1) allow_unverified="y" ;;
        n | N | no | NO | false | FALSE | 0) allow_unverified="n" ;;
        *)
            echo "MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS must be y or n" >&2
            exit 2
            ;;
    esac
    if [[ -n "$trusted_ca" && ! "$trusted_ca" =~ ^[A-Za-z0-9+/=]+$ ]]; then
        echo "MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64 is not valid base64 text" >&2
        exit 2
    fi
    if [[ "$allow_unverified" == n && -z "$trusted_ca" ]]; then
        echo "Strict Remote Control TLS requires MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64" >&2
        exit 2
    fi

    mkdir -p "$build_dir"
    local env_defaults="$build_dir/sdkconfig.remote.defaults"
    local env_defaults_updated
    env_defaults_updated="$(mktemp "${env_defaults}.XXXXXX")"
    {
        printf 'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="%s"\n' "$remote_host"
        printf 'CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=%s\n' "$remote_port"
        if [[ "$allow_unverified" == y ]]; then
            printf 'CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y\n'
        else
            printf '# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set\n'
        fi
        printf 'CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64="%s"\n' "$trusted_ca"
    } >"$env_defaults_updated"
    if [[ ! -f "$env_defaults" ]] || ! cmp -s "$env_defaults_updated" "$env_defaults"; then
        mv "$env_defaults_updated" "$env_defaults"
    else
        rm "$env_defaults_updated"
    fi

    local sdkconfig_path="$build_dir/sdkconfig.release"
    if [[ -f "$sdkconfig_path" ]]; then
        local updated
        updated="$(mktemp "${sdkconfig_path}.XXXXXX")"
        awk -v remote_host="$remote_host" -v remote_port="$remote_port" \
            -v allow_unverified="$allow_unverified" -v trusted_ca="$trusted_ca" \
            -v max_task_name_len="$common_max_task_name_len" \
            -v partial_buffer_height="$partial_buffer_height" '
            BEGIN {
                saw_host = 0; saw_port = 0; saw_tls = 0; saw_ca = 0
                saw_max_task_name_len = 0; saw_partial_buffer_height = 0
            }
            /^CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=/ {
                print "CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=\"" remote_host "\""
                saw_host = 1
                next
            }
            /^CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=/ {
                print "CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=" remote_port
                saw_port = 1
                next
            }
            /^CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=/ ||
            /^# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set$/ {
                if (allow_unverified == "y") print "CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y"
                else print "# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set"
                saw_tls = 1
                next
            }
            /^CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64=/ {
                print "CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64=\"" trusted_ca "\""
                saw_ca = 1
                next
            }
            /^CONFIG_FREERTOS_MAX_TASK_NAME_LEN=/ {
                print "CONFIG_FREERTOS_MAX_TASK_NAME_LEN=" max_task_name_len
                saw_max_task_name_len = 1
                next
            }
            /^CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT=/ {
                if (partial_buffer_height != "") {
                    print "CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT=" partial_buffer_height
                } else {
                    print
                }
                saw_partial_buffer_height = 1
                next
            }
            { print }
            END {
                if (!saw_host) print "CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=\"" remote_host "\""
                if (!saw_port) print "CONFIG_MICROPIXEL_REMOTE_CONTROL_PORT=" remote_port
                if (!saw_tls) {
                    if (allow_unverified == "y") print "CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y"
                    else print "# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set"
                }
                if (!saw_ca) print "CONFIG_MICROPIXEL_REMOTE_CONTROL_TRUSTED_CA_DER_BASE64=\"" trusted_ca "\""
                if (!saw_max_task_name_len) print "CONFIG_FREERTOS_MAX_TASK_NAME_LEN=" max_task_name_len
                if (!saw_partial_buffer_height && partial_buffer_height != "") {
                    print "CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT=" partial_buffer_height
                }
            }
        ' "$sdkconfig_path" >"$updated"
        if ! cmp -s "$updated" "$sdkconfig_path"; then
            mv "$updated" "$sdkconfig_path"
        else
            rm "$updated"
        fi
    fi

    local defaults=("$firmware_dir/sdkconfig.defaults")
    for default_path in "$@"; do
        defaults+=("$firmware_dir/$default_path")
    done
    defaults+=("$env_defaults")
    local joined_defaults
    joined_defaults="$(IFS=';'; echo "${defaults[*]}")"
    profile_action "$profile" build --build-dir "$build_dir" --sdkconfig "$sdkconfig_path" \
        --sdkconfig-defaults "$joined_defaults"
}

resolve_profile_port() {
    local profile="$1"
    local requested="$2"
    local arguments=()
    if [[ -n "$requested" ]]; then
        arguments+=(--port "$requested")
    fi
    if (( ${#arguments[@]} )); then
        profile_action "$profile" port "${arguments[@]}"
    else
        profile_action "$profile" port
    fi
}

is_board_name() {
    case "${1:-}" in
        box3 | esp-box-3 | szpi | szpi-esp32s3 | cores3 | m5stack-cores3) return 0 ;;
        *) return 1 ;;
    esac
}

select_board() {
    case "${1:-box3}" in
        box3 | esp-box-3)
            board_name="box3"
            board_title="ESP32-S3-BOX-3"
            board_profile="esp-box-3"
            board_build_dir="$host_build_dir"
            board_defaults=(sdkconfig.s3.defaults sdkconfig.s3-box-3.defaults)
            board_baud="${S3_BAUD:-921600}"
            ;;
        szpi | szpi-esp32s3)
            board_name="szpi"
            board_title="LCKFB SZPI ESP32-S3"
            board_profile="szpi-esp32s3"
            board_build_dir="$szpi_host_build_dir"
            board_defaults=(sdkconfig.s3.defaults sdkconfig.s3-szpi.defaults)
            board_baud="${SZPI_S3_BAUD:-921600}"
            ;;
        cores3 | m5stack-cores3)
            board_name="cores3"
            board_title="M5Stack CoreS3"
            board_profile="m5stack-cores3"
            board_build_dir="$cores3_host_build_dir"
            board_defaults=(sdkconfig.s3.defaults sdkconfig.s3-cores3.defaults)
            board_baud="${CORES3_S3_BAUD:-921600}"
            ;;
        *)
            echo "Unknown ESP32-S3 board: $1 (expected box3, szpi, or cores3)" >&2
            exit 2
            ;;
    esac
}

split_optional_board() {
    selected_board="box3"
    remaining_arguments=("$@")
    if (( ${#remaining_arguments[@]} > 0 )) && is_board_name "${remaining_arguments[0]}"; then
        selected_board="${remaining_arguments[0]}"
        remaining_arguments=("${remaining_arguments[@]:1}")
    fi
}

build_board() {
    select_board "$1"
    build_profile "$board_profile" "$board_build_dir" "${board_defaults[@]}"
}

flash_board() {
    local selected="$1"
    local requested="${2:-}"
    select_board "$selected"
    local arguments=()
    if [[ -n "$requested" ]]; then arguments+=(--port "$requested"); fi
    profile_action "$board_profile" flash-built "${arguments[@]}"
}

resolve_board_port() {
    local selected="$1"
    local requested="${2:-}"
    select_board "$selected"
    resolve_profile_port "$board_profile" "$requested"
}

build_apps() {
    if [[ ! -x "$xtensa_wamrc" ]]; then
        bash "$workspace_root/tools/build_wamrc_xtensa.sh"
    fi
    local app
    local bundles=()
    mkdir -p "$apps_output_dir"
    for app in demo blocks snake tilt; do
        mkdir -p "$apps_output_dir/$app"
        WAMRC="$xtensa_wamrc" python "$workspace_root/tools/micropixel" package \
            "$workspace_root/guest/apps/$app" \
            --profile release \
            --aot-target xtensa \
            --output-dir "$apps_output_dir/$app" \
            --output "$apps_output_dir/$app.bundle.bin"
        bundles+=("$apps_output_dir/$app.bundle.bin")
    done
    python "$workspace_root/tools/build_app_store_image.py" \
        --app-store-size 0x0800000 \
        --output "$apps_store" \
        "${bundles[@]}"
}

build_release() {
    select_board "$1"
    echo "==> Building shared ESP32-S3 preview Apps: Blocks, Snake, Tilt, and SDK Demo"
    build_apps
    build_profile "$board_profile" "$board_build_dir" "${board_defaults[@]}"
    echo "==> Creating $board_title browser image with Blocks, Snake, Tilt, and SDK Demo"
    python "$workspace_root/tools/build_full_firmware_image.py" \
        --build-dir "$board_build_dir" \
        --app-store-image "$apps_store" \
        --output "$board_build_dir/micropixel-full.bin"
}

flash_apps() {
    local selected="$1"
    local requested="${2:-}"
    local port
    select_board "$selected"
    [[ -f "$apps_store" ]] || build_apps
    port="$(resolve_board_port "$board_name" "$requested")"
    python -m esptool --chip esp32s3 --port "$port" --baud "$board_baud" \
        write-flash 0x800000 "$apps_store"
}

monitor_profile() {
    local profile="$1"
    shift
    local monitor_port=""
    local monitor_reset=""
    for argument in "$@"; do
        if [[ "$argument" == "--reset" ]]; then
            [[ -z "$monitor_reset" ]] || { usage >&2; exit 2; }
            monitor_reset="--reset"
        elif [[ -z "$monitor_port" ]]; then
            monitor_port="$argument"
        else
            usage >&2
            exit 2
        fi
    done
    local arguments=()
    if [[ -n "$monitor_port" ]]; then arguments+=(--port "$monitor_port"); fi
    if [[ -n "$monitor_reset" ]]; then arguments+=("$monitor_reset"); fi
    if (( ${#arguments[@]} )); then
        profile_action "$profile" monitor "${arguments[@]}"
    else
        profile_action "$profile" monitor
    fi
}

command_name="${1:-help}"
shift || true

case "$command_name" in
    help | -h | --help)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        usage
        ;;
    build-null)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        profile_action s3-null build
        ;;
    build-host)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        build_board "${1:-box3}"
        ;;
    build-szpi)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        build_board szpi
        ;;
    build-cores3)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        build_board cores3
        ;;
    build-wamrc)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        bash "$workspace_root/tools/build_wamrc_xtensa.sh"
        ;;
    build-apps)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        build_apps
        ;;
    build-release)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        build_release "${1:-box3}"
        ;;
    flash-host)
        split_optional_board "$@"
        [[ ${#remaining_arguments[@]} -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        flash_board "$selected_board" "${remaining_arguments[0]:-}"
        ;;
    flash-szpi)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        flash_board szpi "${1:-}"
        ;;
    flash-cores3)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        flash_board cores3 "${1:-}"
        ;;
    flash-apps)
        split_optional_board "$@"
        [[ ${#remaining_arguments[@]} -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        flash_apps "$selected_board" "${remaining_arguments[0]:-}"
        ;;
    flash-all)
        split_optional_board "$@"
        [[ ${#remaining_arguments[@]} -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        build_release "$selected_board"
        flash_board "$selected_board" "${remaining_arguments[0]:-}"
        flash_apps "$selected_board" "${remaining_arguments[0]:-}"
        ;;
    monitor)
        split_optional_board "$@"
        [[ ${#remaining_arguments[@]} -le 2 ]] || { usage >&2; exit 2; }
        require_idf
        select_board "$selected_board"
        monitor_profile "$board_profile" "${remaining_arguments[@]}"
        ;;
    monitor-szpi)
        [[ $# -le 2 ]] || { usage >&2; exit 2; }
        require_idf
        monitor_profile szpi-esp32s3 "$@"
        ;;
    monitor-cores3)
        [[ $# -le 2 ]] || { usage >&2; exit 2; }
        require_idf
        monitor_profile m5stack-cores3 "$@"
        ;;
    port)
        split_optional_board "$@"
        [[ ${#remaining_arguments[@]} -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        resolve_board_port "$selected_board" "${remaining_arguments[0]:-}"
        ;;
    port-szpi)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        resolve_profile_port szpi-esp32s3 "${1:-${SZPI_S3_PORT:-}}"
        ;;
    port-cores3)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        resolve_profile_port m5stack-cores3 "${1:-${CORES3_S3_PORT:-}}"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
