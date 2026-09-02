#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
wamr_root="$workspace_root/firmware/espressif/components/wasm-micro-runtime"
llvm_root="$wamr_root/core/deps/llvm/build"
llvm_config="$llvm_root/lib/cmake/llvm/LLVMConfig.cmake"
output_dir="${XTENSA_WAMRC_BUILD_DIR:-$workspace_root/build/tools/wamrc-xtensa}"
case "$(uname -s)" in
    Darwin) host_platform="darwin" ;;
    Linux) host_platform="linux" ;;
    *)
        echo "Xtensa wamrc bootstrap supports macOS and Linux hosts." >&2
        exit 2
        ;;
esac

if [[ ! -f "$llvm_config" ]]; then
    idf_path_override="${IDF_PATH:-}"
    if [[ -f "$workspace_root/.env" ]]; then
        set -a
        # shellcheck disable=SC1091
        source "$workspace_root/.env"
        set +a
    fi
    if [[ -n "$idf_path_override" ]]; then
        IDF_PATH="$idf_path_override"
    fi
    if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
        echo "IDF_PATH is required to prepare the Xtensa LLVM build environment." >&2
        exit 2
    fi
    # shellcheck disable=SC1090
    source "$IDF_PATH/export.sh" >/dev/null
    python "$wamr_root/build-scripts/build_llvm.py" --platform xtensa --arch Xtensa
fi

if [[ ! -f "$llvm_config" ]]; then
    echo "Xtensa LLVM build completed without $llvm_config" >&2
    exit 1
fi

cmake -S "$wamr_root/wamr-compiler" -B "$output_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWAMR_BUILD_PLATFORM="$host_platform" \
    -DWAMR_BUILD_WITH_CUSTOM_LLVM=1 \
    -DWAMR_BUILD_SIMD=0 \
    -DLLVM_DIR="$llvm_root/lib/cmake/llvm"
cmake --build "$output_dir" --parallel

"$output_dir/wamrc" --version
echo "Xtensa-capable wamrc ready: $output_dir/wamrc"
