#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
idf_path_override="${IDF_PATH:-}"
s31_port_override="${S31_PORT:-}"
s31_baud_override="${S31_BAUD:-}"
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
if [[ -n "$s31_port_override" ]]; then
    S31_PORT="$s31_port_override"
fi
if [[ -n "$s31_baud_override" ]]; then
    S31_BAUD="$s31_baud_override"
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
host_build_dir="${S31_HOST_BUILD_DIR:-$workspace_root/build/host-esp32s31-mosaico}"
sdkconfig_path="${S31_SDKCONFIG:-$host_build_dir/sdkconfig.release}"
sdkconfig_defaults="${S31_SDKCONFIG_DEFAULTS:-$firmware_dir/sdkconfig.defaults;$firmware_dir/sdkconfig.s31.defaults}"
system_shell_output_dir="${S31_SYSTEM_SHELL_OUTPUT_DIR:-$workspace_root/build/system-shell-s31}"
release_app_store_image="$system_shell_output_dir/app-store-release.bin"
host_config_prepared=false

usage() {
    cat <<'EOF'
Usage: bash tools/s31.sh COMMAND [PORT] [--reset]

ESP32-S31 board aliases backed by the common firmware profile tool:
  build-host      Incrementally build only the ESP-Mosaico Host.
  build-release   Build the Host and the three release Apps, then create a
                  browser-flashable full image for ESP-Mosaico.
  flash-host      Flash the already-built Host; preserve app_store and do not
                  rebuild or rewrite SDK Demo/App Bundles.
  build-mosaico   Compatibility alias for build-host.
  build-null      Build the hardware-independent Runtime for ESP32-S31.
  fullclean-mosaico
                   Delete only the ESP-Mosaico generated build cache.
  flash-mosaico   Deprecated compatibility alias for flash-host; no build.
  monitor         Monitor without rebuilding; pass --reset to capture boot logs.

The ESP-Mosaico profile enables the CO5300 display, 78/esp_lcd_touch_cst92xx touch,
ES8311 audio, BMI270 plus dual BMM150 sensors, battery/power-key handling,
safe expansion GPIO, native Wi-Fi and the shared Runtime. NAND, microphone
capture and module discovery remain outside the current first-stage scope.

S31_PORT and S31_BAUD can be set in the repository-root .env. An explicit PORT
argument takes precedence; the default flash baud is 460800. Once MicroPixel
has been flashed once, flash-host switches its application CDC into ROM
download mode and follows the USB re-enumeration automatically.

build-host also reads MICROPIXEL_REMOTE_CONTROL_HOST, PORT,
ALLOW_UNVERIFIED_TLS and TRUSTED_CA_DER_BASE64 from .env, matching the P4 Host
configuration behavior.
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
        local previous_environment="${CONDA_DEFAULT_ENV:-unknown}"
        eval "$("$CONDA_EXE" shell.bash hook 2>/dev/null)"
        while (( ${CONDA_SHLVL:-0} > 0 )); do
            conda deactivate
        done
        echo "==> Conda environment disabled for this S31 command: $previous_environment"
    fi
    if [[ "$(command -v idf.py 2>/dev/null || true)" != "$IDF_PATH/tools/idf.py" ]]; then
        # shellcheck disable=SC1090
        source "$IDF_PATH/export.sh" >/dev/null
    fi
}

prepare_host_config() {
    if $host_config_prepared; then
        return
    fi
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

    mkdir -p "$host_build_dir"
    local env_defaults="$host_build_dir/sdkconfig.env.defaults"
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
        printf '# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set\n'
        printf 'CONFIG_PM_ENABLE=y\n'
        printf '# CONFIG_PM_DFS_INIT_AUTO is not set\n'
        printf 'CONFIG_FREERTOS_USE_TICKLESS_IDLE=y\n'
        printf 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y\n'
    } >"$env_defaults_updated"
    if [[ ! -f "$env_defaults" ]] || ! cmp -s "$env_defaults_updated" "$env_defaults"; then
        mv "$env_defaults_updated" "$env_defaults"
    else
        rm "$env_defaults_updated"
    fi
    sdkconfig_defaults="$sdkconfig_defaults;$env_defaults"
    export S31_SDKCONFIG_DEFAULTS="$sdkconfig_defaults"

    # Kconfig defaults do not replace values already materialized in the
    # incremental build directory. Refresh machine-local Remote Control fields
    # and the S31 performance/power baseline without requiring a fullclean.
    if [[ -f "$sdkconfig_path" ]]; then
        local updated
        updated="$(mktemp "${sdkconfig_path}.XXXXXX")"
        awk -v remote_host="$remote_host" -v remote_port="$remote_port" \
            -v allow_unverified="$allow_unverified" -v trusted_ca="$trusted_ca" \
            -v max_task_name_len="$common_max_task_name_len" '
            BEGIN {
                saw_host = 0; saw_port = 0; saw_tls = 0; saw_ca = 0; saw_cert_time = 0
                saw_pm = 0; saw_pm_dfs = 0; saw_tickless = 0; saw_wifi_lwip_psram = 0
                saw_main_stack = 0; saw_max_task_name_len = 0
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
            /^CONFIG_MBEDTLS_HAVE_TIME_DATE=/ || /^# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set$/ {
                print "# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set"
                saw_cert_time = 1
                next
            }
            /^CONFIG_PM_ENABLE=/ || /^# CONFIG_PM_ENABLE is not set$/ {
                print "CONFIG_PM_ENABLE=y"
                saw_pm = 1
                next
            }
            /^CONFIG_PM_DFS_INIT_AUTO=/ || /^# CONFIG_PM_DFS_INIT_AUTO is not set$/ {
                print "# CONFIG_PM_DFS_INIT_AUTO is not set"
                saw_pm_dfs = 1
                next
            }
            /^CONFIG_FREERTOS_USE_TICKLESS_IDLE=/ || /^# CONFIG_FREERTOS_USE_TICKLESS_IDLE is not set$/ {
                print "CONFIG_FREERTOS_USE_TICKLESS_IDLE=y"
                saw_tickless = 1
                next
            }
            /^CONFIG_FREERTOS_MAX_TASK_NAME_LEN=/ {
                print "CONFIG_FREERTOS_MAX_TASK_NAME_LEN=" max_task_name_len
                saw_max_task_name_len = 1
                next
            }
            /^CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=/ || /^# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set$/ {
                print "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y"
                saw_wifi_lwip_psram = 1
                next
            }
            /^CONFIG_ESP_MAIN_TASK_STACK_SIZE=/ {
                print "CONFIG_ESP_MAIN_TASK_STACK_SIZE=14336"
                saw_main_stack = 1
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
                if (!saw_cert_time) print "# CONFIG_MBEDTLS_HAVE_TIME_DATE is not set"
                if (!saw_pm) print "CONFIG_PM_ENABLE=y"
                if (!saw_pm_dfs) print "# CONFIG_PM_DFS_INIT_AUTO is not set"
                if (!saw_tickless) print "CONFIG_FREERTOS_USE_TICKLESS_IDLE=y"
                if (!saw_wifi_lwip_psram) print "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y"
                if (!saw_main_stack) print "CONFIG_ESP_MAIN_TASK_STACK_SIZE=14336"
                if (!saw_max_task_name_len) print "CONFIG_FREERTOS_MAX_TASK_NAME_LEN=" max_task_name_len
            }
        ' "$sdkconfig_path" >"$updated"
        if ! cmp -s "$updated" "$sdkconfig_path"; then
            mv "$updated" "$sdkconfig_path"
        else
            rm "$updated"
        fi
    fi
    host_config_prepared=true
}

build_profile() {
    local profile="$1"
    echo "==> ESP32-S31 preview compile gate: $profile (build only)"
    if [[ "$profile" == "esp-mosaico" ]]; then
        prepare_host_config
        python3 "$workspace_root/tools/firmware.py" "$profile" build \
            --build-dir "$host_build_dir" \
            --sdkconfig "$sdkconfig_path" \
            --sdkconfig-defaults "$sdkconfig_defaults"
    else
        python3 "$workspace_root/tools/firmware.py" "$profile" build
    fi
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

build_release() {
    echo "==> Building ESP-Mosaico release Apps: Blocks, Snake, and SDK Demo"
    build_app_package blocks
    build_app_package snake
    build_app_package demo
    build_profile esp-mosaico
    mkdir -p "$system_shell_output_dir"
    python3 "$workspace_root/tools/build_app_store_image.py" \
        --app-store-size 0x0800000 \
        --output "$release_app_store_image" \
        "$workspace_root/build/apps/blocks/blocks.bundle.bin" \
        "$workspace_root/build/apps/snake/snake.bundle.bin" \
        "$workspace_root/build/apps/demo/demo.bundle.bin"
    echo "==> Creating ESP-Mosaico browser image with Blocks, Snake, and SDK Demo"
    python3 "$workspace_root/tools/build_full_firmware_image.py" \
        --build-dir "$host_build_dir" \
        --app-store-image "$release_app_store_image" \
        --output "$host_build_dir/micropixel-full.bin"
}

run_serial_action() {
    local action="$1"
    local requested_port="$2"
    local arguments=(esp-mosaico "$action")
    if [[ -n "$requested_port" ]]; then
        arguments+=(--port "$requested_port")
    fi
    python3 "$workspace_root/tools/firmware.py" "${arguments[@]}"
}

command_name="${1:-help}"
shift || true

case "$command_name" in
    help | -h | --help)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        usage
        ;;
    build-host | build-mosaico)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        build_profile esp-mosaico
        ;;
    build-release)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        build_release
        ;;
    build-null)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        build_profile s31-null
        ;;
    fullclean-mosaico)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        python3 "$workspace_root/tools/firmware.py" esp-mosaico fullclean
        ;;
    flash-mosaico)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        echo "==> flash-mosaico is deprecated; using flash-host (no build, app_store preserved)"
        run_serial_action flash-built "${1:-}"
        ;;
    flash-host)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        require_idf
        run_serial_action flash-built "${1:-}"
        ;;
    monitor)
        [[ $# -le 2 ]] || { usage >&2; exit 2; }
        require_idf
        monitor_port=""
        monitor_reset=""
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
        arguments=(esp-mosaico monitor)
        if [[ -n "$monitor_port" ]]; then
            arguments+=(--port "$monitor_port")
        fi
        if [[ -n "$monitor_reset" ]]; then
            arguments+=("$monitor_reset")
        fi
        python3 "$workspace_root/tools/firmware.py" "${arguments[@]}"
        ;;
    *)
        echo "Unknown command: $command_name" >&2
        usage >&2
        exit 2
        ;;
esac
