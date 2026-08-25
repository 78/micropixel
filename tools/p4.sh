#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"

# Load machine-local defaults without overriding values supplied by the caller.
idf_path_override="${IDF_PATH:-}"
p4_port_override="${P4_PORT:-}"
if [[ -f "$workspace_root/.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$workspace_root/.env"
    set +a
fi
if [[ -n "$idf_path_override" ]]; then
    IDF_PATH="$idf_path_override"
fi
if [[ -n "$p4_port_override" ]]; then
    P4_PORT="$p4_port_override"
fi

firmware_dir="$workspace_root/firmware/espressif"
host_build_dir="${P4_HOST_BUILD_DIR:-$workspace_root/build/host-esp32p4}"
system_shell_output_dir="${P4_SYSTEM_SHELL_OUTPUT_DIR:-$workspace_root/build/system-shell-p4}"
example_app_store_image="$system_shell_output_dir/app-store.bin"
idf_environment_cache="$workspace_root/build/p4-idf-environment.sh"
sdkconfig_path="${P4_SDKCONFIG:-$host_build_dir/sdkconfig.release}"
sdkconfig_defaults="${P4_SDKCONFIG_DEFAULTS:-$firmware_dir/sdkconfig.p4.defaults}"
host_config_prepared=false

usage() {
    cat <<'EOF'
Usage: bash tools/p4.sh COMMAND [ARGUMENTS]

Local defaults are loaded from the repository-root .env. Explicit environment
variables and command-line PORT arguments take precedence.

Normal development commands:
  build-host                         Incrementally build only the ESP32-P4 Host.
  flash-host [PORT]                  Incrementally build and flash only the Host;
                                     preserve the app_store partition.
  monitor [PORT]                     Monitor the running ESP32-P4 Host without
                                     building, flashing, erasing, or testing.
  build-apps                         Build Blocks, Snake, and Demo Bundles.
  build-all                          Build Host plus Blocks, Snake, Demo, and the
                                     App Store image. Run no tests; flash nothing.
  flash-apps [PORT]                  Clear app_store and flash Blocks, Snake, Demo
                                     over USB. Uses the unique connected ESP32-P4
                                     when PORT is omitted.

Explicit full/destructive commands:
  fullclean-host                     Delete the Host build cache with idf.py fullclean.
  flash-all [PORT]                   Build and flash the Host, then clear and flash
                                     Blocks, Snake, Demo. Run no tests.
  reset-app-store [PORT]             Recovery only: clear app_store to an EMPTY Catalog.
  install-apps-examples              Non-USB alternative: install examples through
                                     Remote Control while preserving other Apps.

Release/pre-push command:
  test                               Explicitly run builds, conformance, Host tests,
                                     Bundle tests, audio tests, format, and shell checks.

install-apps-examples requires these environment variables:
  MICROPIXEL_DEVICE_ID               Paired device UUID.
  MICROPIXEL_CONTROL_TOKEN           Control JWT with app:install and device:read.
  MICROPIXEL_CONTROL_URL             Optional; defaults to https://localhost:8443.

Examples:
  bash tools/p4.sh build-host
  bash tools/p4.sh flash-host
  bash tools/p4.sh monitor
  bash tools/p4.sh flash-apps
  bash tools/p4.sh flash-all
  bash tools/p4.sh test
  MICROPIXEL_DEVICE_ID=... MICROPIXEL_CONTROL_TOKEN=... \
    bash tools/p4.sh install-apps-examples
EOF
}

deactivate_conda_for_p4() {
    if [[ "${CONDA_SHLVL:-0}" == 0 ]]; then
        return
    fi
    if [[ -z "${CONDA_EXE:-}" || ! -x "$CONDA_EXE" ]]; then
        echo "Conda is active, but CONDA_EXE is unavailable; run 'conda deactivate' first." >&2
        exit 2
    fi

    local previous_environment="${CONDA_DEFAULT_ENV:-unknown}"
    # Install Conda's shell function in this p4.sh process, then unwind every
    # stacked environment. An executed script cannot alter its parent shell.
    eval "$("$CONDA_EXE" shell.bash hook 2>/dev/null)"
    while (( ${CONDA_SHLVL:-0} > 0 )); do
        conda deactivate
    done
    echo "==> Conda environment disabled for this P4 command: $previous_environment"
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
        echo "IDF_PATH is missing or invalid; set it in the repository-root .env." >&2
        exit 2
    fi

    deactivate_conda_for_p4

    # Reuse a matching environment that the caller already activated.
    if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]] &&
        [[ "$(command -v idf.py 2>/dev/null || true)" == "$IDF_PATH/tools/idf.py" ]]; then
        return
    fi

    local refresh_cache=false
    if [[ ! -f "$idf_environment_cache" ]] ||
        [[ "$workspace_root/tools/p4.sh" -nt "$idf_environment_cache" ]] ||
        [[ "$IDF_PATH/export.sh" -nt "$idf_environment_cache" ]] ||
        [[ "$IDF_PATH/tools/activate.py" -nt "$idf_environment_cache" ]] ||
        [[ "$IDF_PATH/tools/tools.json" -nt "$idf_environment_cache" ]] ||
        ! grep -Fqx "export IDF_PATH=\"$IDF_PATH\"" "$idf_environment_cache"; then
        refresh_cache=true
    fi

    if $refresh_cache; then
        local activation_line activation_file updated_cache
        mkdir -p "$(dirname "$idf_environment_cache")"
        echo "==> Preparing cached ESP-IDF 6.1 environment (first run or IDF update)"
        activation_line="$(python3 "$IDF_PATH/tools/activate.py" --export --shell bash --quiet)"
        activation_file="${activation_line#. }"
        if [[ "$activation_file" == "$activation_line" || ! -f "$activation_file" ]]; then
            echo "ESP-IDF activation did not produce a readable environment file." >&2
            exit 2
        fi
        updated_cache="$(mktemp "${idf_environment_cache}.XXXXXX")"
        # The official activation file starts with all required exports. The
        # later completion setup and welcome text are interactive-shell noise.
        awk '
            NF == 0 { if (started) exit; next }
            /^export IDF_DEACTIVATE_FILE_PATH=/ { next }
            { started = 1; print }
        ' \
            "$activation_file" >"$updated_cache"
        mv "$updated_cache" "$idf_environment_cache"
    fi

    # shellcheck disable=SC1090
    source "$idf_environment_cache"
    if [[ -z "${IDF_PYTHON_ENV_PATH:-}" || ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]] ||
        [[ "$(command -v idf.py 2>/dev/null || true)" != "$IDF_PATH/tools/idf.py" ]]; then
        echo "Cached ESP-IDF environment is invalid; remove $idf_environment_cache and retry." >&2
        exit 2
    fi
}

prepare_host_config() {
    if $host_config_prepared; then
        return
    fi
    local remote_host="${MICROPIXEL_REMOTE_CONTROL_HOST:-}"
    if [[ -n "$remote_host" && ! "$remote_host" =~ ^[A-Za-z0-9._:-]+$ ]]; then
        echo "MICROPIXEL_REMOTE_CONTROL_HOST contains unsupported characters: $remote_host" >&2
        exit 2
    fi

    mkdir -p "$host_build_dir"
    local env_defaults="$host_build_dir/sdkconfig.env.defaults"
    local env_defaults_updated
    env_defaults_updated="$(mktemp "${env_defaults}.XXXXXX")"
    {
        printf 'CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST="%s"\n' "$remote_host"
        printf 'CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y\n'
    } >"$env_defaults_updated"
    if [[ ! -f "$env_defaults" ]] || ! cmp -s "$env_defaults_updated" "$env_defaults"; then
        mv "$env_defaults_updated" "$env_defaults"
    else
        rm "$env_defaults_updated"
    fi
    sdkconfig_defaults="$sdkconfig_defaults;$env_defaults"
    export P4_SDKCONFIG_DEFAULTS="$sdkconfig_defaults"

    # sdkconfig defaults do not override values already materialized by Kconfig.
    # Refresh the generated build config as well, so changing .env takes effect
    # without requiring fullclean.
    if [[ -f "$sdkconfig_path" ]]; then
        local updated
        updated="$(mktemp "${sdkconfig_path}.XXXXXX")"
        awk -v remote_host="$remote_host" '
            BEGIN { saw_host = 0; saw_tls = 0; saw_hw_ecdsa = 0; saw_cert_bundle = 0; saw_ota_rollback = 0 }
            /^CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=/ {
                print "CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=\"" remote_host "\""
                saw_host = 1
                next
            }
            /^CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=/ ||
            /^# CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS is not set$/ {
                print "CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y"
                saw_tls = 1
                next
            }
            /^CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY=/ ||
            /^# CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY is not set$/ {
                print "# CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY is not set"
                saw_hw_ecdsa = 1
                next
            }
            /^CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=/ ||
            /^# CONFIG_MBEDTLS_CERTIFICATE_BUNDLE is not set$/ {
                print "# CONFIG_MBEDTLS_CERTIFICATE_BUNDLE is not set"
                saw_cert_bundle = 1
                next
            }
            /^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=/ ||
            /^# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set$/ {
                print "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
                saw_ota_rollback = 1
                next
            }
            { print }
            END {
                if (!saw_host) print "CONFIG_MICROPIXEL_REMOTE_CONTROL_HOST=\"" remote_host "\""
                if (!saw_tls) print "CONFIG_MICROPIXEL_REMOTE_CONTROL_ALLOW_UNVERIFIED_TLS=y"
                if (!saw_hw_ecdsa) print "# CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY is not set"
                if (!saw_cert_bundle) print "# CONFIG_MBEDTLS_CERTIFICATE_BUNDLE is not set"
                if (!saw_ota_rollback) print "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
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

idf_host() {
    prepare_host_config
    idf.py -C "$firmware_dir" -B "$host_build_dir" \
        -D SDKCONFIG="$sdkconfig_path" \
        -D SDKCONFIG_DEFAULTS="$sdkconfig_defaults" \
        -D IDF_TARGET=esp32p4 \
        "$@"
}

validate_port() {
    local port="$1"
    if [[ ! -e "$port" ]]; then
        echo "Serial port not found: $port" >&2
        exit 2
    fi
}

resolve_port() {
    local requested="${1:-${P4_PORT:-}}"
    if [[ -n "$requested" ]]; then
        validate_port "$requested"
        printf '%s\n' "$requested"
        return
    fi

    require_idf
    local matches=()
    local candidate probe
    for candidate in /dev/cu.usbmodem* /dev/ttyACM*; do
        [[ -e "$candidate" ]] || continue
        probe="$(python -m esptool --port "$candidate" chip-id 2>&1 || true)"
        if [[ "$probe" == *"ESP32-P4"* ]]; then
            matches+=("$candidate")
        fi
    done
    if [[ ${#matches[@]} -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return
    fi
    if [[ ${#matches[@]} -eq 0 ]]; then
        echo "No connected ESP32-P4 USB device was detected; set P4_PORT explicitly." >&2
    else
        echo "Multiple ESP32-P4 USB devices detected; set P4_PORT explicitly:" >&2
        printf '  %s\n' "${matches[@]}" >&2
    fi
    exit 2
}

build_host() {
    require_idf
    echo "==> Incremental Host build (no Guest builds, no unit tests, no fullclean)"
    idf_host build
}

flash_host() {
    local port="$1"
    local baud="${P4_BAUD:-2000000}"
    build_host
    echo "==> Flashing Host at $baud baud; app_store and installed Apps are preserved"
    idf_host -b "$baud" -p "$port" flash
}

monitor_host() {
    local port="$1"
    if [[ ! -f "$host_build_dir/micropixel.elf" ]]; then
        echo "Host ELF missing: $host_build_dir/micropixel.elf" >&2
        echo "Build it first with: bash tools/p4.sh build-host" >&2
        exit 2
    fi
    require_idf
    echo "==> Monitoring Host on $port (no build, no flash, no erase, no tests)"
    idf_host -p "$port" monitor
}

build_example_apps() {
    echo "==> Building example Apps: Blocks, Snake, Demo"
    bash "$workspace_root/tools/build_blocks_bundle.sh"
    bash "$workspace_root/tools/build_snake_bundle.sh"
    bash "$workspace_root/tools/build_demo_bundle.sh"
}

app_store_partition_info() {
    local partition_table="$host_build_dir/partition_table/partition-table.bin"
    local partition_name="${P4_GUEST_PARTITION:-app_store}"
    if [[ ! -f "$partition_table" ]]; then
        echo "P4 partition table missing: $partition_table" >&2
        echo "Build the Host first with: bash tools/p4.sh build-host" >&2
        return 2
    fi
    python "$IDF_PATH/components/partition_table/parttool.py" \
        --partition-table-file "$partition_table" \
        get_partition_info --partition-name "$partition_name" --info offset size
}

create_example_app_store_image() {
    local bundles=(
        "$workspace_root/build/apps/blocks/blocks.bundle.bin"
        "$workspace_root/build/apps/snake/snake.bundle.bin"
        "$workspace_root/build/apps/demo/demo.bundle.bin"
    )
    local bundle
    for bundle in "${bundles[@]}"; do
        if [[ ! -f "$bundle" ]]; then
            echo "Example Bundle missing: $bundle" >&2
            echo "Build them first with: bash tools/p4.sh build-apps" >&2
            return 2
        fi
    done
    mkdir -p "$system_shell_output_dir"
    python3 "$workspace_root/tools/build_app_store_image.py" \
        --output "$example_app_store_image" "${bundles[@]}"
}

write_app_store_image() {
    local port="$1"
    local image="$2"
    local verify="${3:-false}"
    local baud="${P4_BAUD:-2000000}"
    local partition_offset partition_size image_size
    read -r partition_offset partition_size < <(app_store_partition_info)
    image_size="$(wc -c < "$image" | tr -d ' ')"
    if (( image_size > partition_size )); then
        echo "App Store image is larger than app_store ($image_size > $partition_size)." >&2
        return 2
    fi
    python -m esptool --chip esp32p4 --port "$port" --baud "$baud" \
        write-flash "$partition_offset" "$image"
    if [[ "$verify" == true ]]; then
        python -m esptool --chip esp32p4 --port "$port" --baud "$baud" \
            verify-flash "$partition_offset" "$image"
    fi
}

build_all() {
    echo "==> Building Host + Blocks, Snake, Demo, and App Store image (no tests, no flash)"
    build_example_apps
    build_host
    create_example_app_store_image
    local partition_offset partition_size image_size
    read -r partition_offset partition_size < <(app_store_partition_info)
    image_size="$(wc -c < "$example_app_store_image" | tr -d ' ')"
    if (( image_size > partition_size )); then
        echo "App Store image is larger than app_store ($image_size > $partition_size)." >&2
        return 2
    fi
    echo "==> App Store image ready: $example_app_store_image (Blocks, Snake, Demo)"
}

flash_all() {
    local port="$1"
    local baud="${P4_BAUD:-2000000}"
    build_all
    echo "==> Flashing Host at $baud baud"
    idf_host -b "$baud" -p "$port" flash
    echo "==> Clearing app_store and flashing Blocks, Snake, Demo"
    write_app_store_image "$port" "$example_app_store_image" true
    python "$workspace_root/tools/capture_serial_until.py" \
        "$port" "System Shell ready: App Hall rendered" --timeout 30 --reset
    echo "System Shell P4 flashed and verified on $port with Blocks, Snake, and Demo."
}

install_bundle() {
    local bundle="$1"
    local app_name="$2"
    local base_url="$MICROPIXEL_CONTROL_URL"
    local device_id="$MICROPIXEL_DEVICE_ID"
    local token="$MICROPIXEL_CONTROL_TOKEN"
    local package_json package_id job_json job_id job_json_status

    echo "==> Uploading $app_name through the normal App Store API"
    package_json="$(curl --http3 --fail-with-body -sS -X POST \
        -H "Authorization: Bearer $token" \
        -H "Content-Type: application/vnd.micropixel.app" \
        --data-binary "@$bundle" \
        "$base_url/api/v1/devices/$device_id/packages")"
    package_id="$(jq -er '.packageId' <<<"$package_json")"
    job_json="$(curl --http3 --fail-with-body -sS -X POST \
        -H "Authorization: Bearer $token" \
        -H "Content-Type: application/json" \
        -d "{\"packageId\":\"$package_id\"}" \
        "$base_url/api/v1/devices/$device_id/apps:install")"
    job_id="$(jq -er '.id' <<<"$job_json")"

    for _ in {1..300}; do
        job_json="$(curl --http3 --fail-with-body -sS \
            -H "Authorization: Bearer $token" \
            "$base_url/api/v1/devices/$device_id/jobs/$job_id")"
        job_json_status="$(jq -er '.status' <<<"$job_json")"
        case "$job_json_status" in
            acknowledged)
                echo "    $app_name installed (job $job_id)"
                return 0
                ;;
            failed | expired)
                echo "$app_name installation $job_json_status: $job_json" >&2
                return 1
                ;;
            queued | sent)
                sleep 1
                ;;
            *)
                echo "Unexpected install status for $app_name: $job_json" >&2
                return 1
                ;;
        esac
    done
    echo "$app_name installation timed out after 300 seconds (job $job_id)." >&2
    return 1
}

install_example_apps() {
    : "${MICROPIXEL_DEVICE_ID:?Set MICROPIXEL_DEVICE_ID to the paired device UUID}"
    : "${MICROPIXEL_CONTROL_TOKEN:?Set MICROPIXEL_CONTROL_TOKEN to a control JWT}"
    MICROPIXEL_CONTROL_URL="${MICROPIXEL_CONTROL_URL:-https://localhost:8443}"
    export MICROPIXEL_CONTROL_URL
    if ! command -v curl >/dev/null 2>&1 || ! command -v jq >/dev/null 2>&1; then
        echo "install-apps-examples requires curl and jq." >&2
        exit 2
    fi

    build_example_apps
    echo "==> Installing example Apps; this is not a raw app_store flash"
    install_bundle "$workspace_root/build/apps/blocks/blocks.bundle.bin" Blocks
    install_bundle "$workspace_root/build/apps/snake/snake.bundle.bin" Snake
    install_bundle "$workspace_root/build/apps/demo/demo.bundle.bin" Demo
}

run_tests() {
    echo "==> Explicit release/pre-push test suite"
    bash "$workspace_root/tools/build_guest_p4.sh"
    build_all
    bash "$workspace_root/tools/check_firmware_style.sh" --format-only
    bash "$workspace_root/tools/tests/test_firmware_host.sh"
    PYTHONPATH="$workspace_root${PYTHONPATH:+:$PYTHONPATH}" \
        python3 -m unittest tools.tests.test_build_app_store_image tools.tests.test_analyze_sfx -v
    bash "$workspace_root/tools/tests/test_bundle_reader.sh" \
        "$workspace_root/build/apps/blocks/blocks.bundle.bin" \
        "$workspace_root/build/apps/snake/snake.bundle.bin" \
        "$workspace_root/build/apps/demo/demo.bundle.bin"
    bash -n "$workspace_root"/tools/*.sh
    echo "P4 release/pre-push test suite passed."
}

flash_example_apps() {
    local port="$1"
    require_idf
    create_example_app_store_image
    echo "==> USB App flash: clearing app_store and writing Blocks, Snake, Demo"
    write_app_store_image "$port" "$example_app_store_image"
}

reset_app_store() (
    local port="$1"
    local temporary_dir
    require_idf
    temporary_dir="$(mktemp -d)"
    trap 'rm -rf "$temporary_dir"' EXIT
    python3 "$workspace_root/tools/build_app_store_image.py" --output "$temporary_dir/app-store.bin"
    echo "==> RECOVERY: clearing app_store to an EMPTY Catalog"
    write_app_store_image "$port" "$temporary_dir/app-store.bin"
)

command_name="${1:-help}"
shift || true

case "$command_name" in
    help | -h | --help)
        usage
        ;;
    build-host)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        build_host
        ;;
    flash-host)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        port="$(resolve_port "${1:-}")"
        flash_host "$port"
        ;;
    monitor)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        port="$(resolve_port "${1:-}")"
        monitor_host "$port"
        ;;
    fullclean-host)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        require_idf
        echo "==> FULLCLEAN Host build cache: $host_build_dir"
        idf_host fullclean
        ;;
    build-apps)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        build_example_apps
        ;;
    flash-apps)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        port="$(resolve_port "${1:-}")"
        flash_example_apps "$port"
        ;;
    install-apps-examples)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        install_example_apps
        ;;
    build-all)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        build_all
        ;;
    flash-all)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        port="$(resolve_port "${1:-}")"
        flash_all "$port"
        ;;
    reset-app-store)
        [[ $# -le 1 ]] || { usage >&2; exit 2; }
        port="$(resolve_port "${1:-}")"
        reset_app_store "$port"
        ;;
    test)
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        run_tests
        ;;
    *)
        echo "Unknown command: $command_name" >&2
        usage >&2
        exit 2
        ;;
esac
