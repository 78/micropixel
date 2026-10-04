#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"

idf_path_override="${IDF_PATH:-}"
tab5_port_override="${TAB5_PORT:-}"
tab5_baud_override="${TAB5_BAUD:-}"
if [[ -f "$workspace_root/.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "$workspace_root/.env"
    set +a
fi
if [[ -n "$idf_path_override" ]]; then
    IDF_PATH="$idf_path_override"
fi
if [[ -n "$tab5_port_override" ]]; then
    TAB5_PORT="$tab5_port_override"
fi
if [[ -n "$tab5_baud_override" ]]; then
    TAB5_BAUD="$tab5_baud_override"
fi

usage() {
    cat <<'EOF'
Usage: bash tools/tab5.sh COMMAND [PORT]

Local defaults are loaded from the repository-root .env; explicit environment
variables take precedence.

Commands:
  build-host        Build the Host firmware for M5Stack Tab5
  flash-host [PORT] Build-independent flash of the existing build output
  monitor [PORT]    Attach to the serial monitor and reset the target
  fullclean-host    Delete the Tab5 build directory
  port [PORT]       Identify the chip and MAC on the given or detected port
EOF
}

require_idf() {
    if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
        echo "IDF_PATH is missing or invalid; set it in the repository-root .env." >&2
        exit 2
    fi

    # Reuse a matching environment that the caller already activated.
    if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]] &&
        [[ "$(command -v idf.py 2>/dev/null || true)" == "$IDF_PATH/tools/idf.py" ]]; then
        return
    fi

    # shellcheck disable=SC1091
    source "$IDF_PATH/export.sh" >/dev/null
}

tab5() {
    require_idf
    python3 "$workspace_root/tools/firmware.py" m5stack-tab5 "$@"
}

command="${1:-}"
if [[ $# -gt 0 ]]; then
    shift
fi

case "$command" in
    build-host)
        tab5 build
        ;;
    flash-host)
        arguments=(flash-built)
        if [[ -n "${1:-}" ]]; then
            arguments+=(--port "$1")
        fi
        tab5 "${arguments[@]}"
        ;;
    monitor)
        arguments=(monitor --reset)
        if [[ -n "${1:-}" ]]; then
            arguments+=(--port "$1")
        fi
        tab5 "${arguments[@]}"
        ;;
    fullclean-host)
        tab5 fullclean
        ;;
    port)
        arguments=(port)
        if [[ -n "${1:-}" ]]; then
            arguments+=(--port "$1")
        fi
        tab5 "${arguments[@]}"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
