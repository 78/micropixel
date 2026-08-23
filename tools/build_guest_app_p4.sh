#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: bash tools/build_guest_app_p4.sh SOURCE.cpp [APP_NAME [EXTRA_SOURCE.cpp ...]]" >&2
    exit 2
fi

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
source_path="$1"
app_name="${2:-$(basename "${source_path%.*}")}"
source_paths=("$source_path")
if [[ $# -gt 2 ]]; then
    source_paths+=("${@:3}")
fi
output_dir="${P4_GUEST_OUTPUT_DIR:-$workspace_root/build/guest-p4}"
build_profile="${MICROPIXEL_GUEST_PROFILE:-development}"
generated_include_dir="${MICROPIXEL_GUEST_GENERATED_INCLUDE_DIR:-}"

for guest_source in "${source_paths[@]}"; do
    if [[ ! -f "$guest_source" ]]; then
        echo "Guest source not found: $guest_source" >&2
        exit 2
    fi
done
if [[ ! "$app_name" =~ ^[a-zA-Z0-9_-]+$ ]]; then
    echo "APP_NAME may contain only letters, digits, underscores, and hyphens." >&2
    exit 2
fi
if [[ -n "$generated_include_dir" && ! -d "$generated_include_dir" ]]; then
    echo "Generated Guest include directory not found: $generated_include_dir" >&2
    exit 2
fi

if [[ -n "${WASI_CLANGXX:-}" ]]; then
    wasm_clangxx="$WASI_CLANGXX"
elif [[ -n "${WASI_CLANG:-}" ]]; then
    wasm_clangxx="${WASI_CLANG%clang}clang++"
elif [[ -n "${WASI_SDK_PATH:-}" ]]; then
    wasm_clangxx="$WASI_SDK_PATH/bin/clang++"
elif [[ -n "${HOME:-}" && -x "$HOME/.local/wasi-sdk/bin/clang++" ]]; then
    wasm_clangxx="$HOME/.local/wasi-sdk/bin/clang++"
else
    echo "Set WASI_CLANGXX or WASI_SDK_PATH to a C++23 compiler with the wasm32 backend." >&2
    exit 2
fi

wamrc="${WAMRC:-$workspace_root/build/tools/wamrc-2.4.5-riscv32-ilp32f}"
if [[ ! -x "$wasm_clangxx" ]]; then
    echo "WASI Clang++ is not executable: $wasm_clangxx" >&2
    exit 2
fi
if [[ ! -x "$wamrc" ]]; then
    echo "Set WAMRC to a WAMR 2.4.5 compiler built for RISCV32_ILP32F." >&2
    exit 2
fi

mkdir -p "$output_dir"

case "$build_profile" in
development)
    compile_profile_flags=(-O1 -g)
    link_profile_flags=()
    aot_profile_flags=(--opt-level=1 --size-level=3 --enable-dump-call-stack)
    ;;
release)
    compile_profile_flags=(-O2)
    link_profile_flags=(-Wl,--strip-all)
    aot_profile_flags=(--opt-level=3 --size-level=3 --enable-dump-call-stack)
    ;;
size)
    compile_profile_flags=(-Oz)
    link_profile_flags=(-Wl,--strip-all)
    aot_profile_flags=(--opt-level=3 --size-level=3 --enable-dump-call-stack)
    ;;
*)
    echo "MICROPIXEL_GUEST_PROFILE must be development, release, or size." >&2
    exit 2
    ;;
esac

guest_include_flags=(-I "$workspace_root/guest")
if [[ -n "$generated_include_dir" ]]; then
    guest_include_flags+=(-I "$generated_include_dir")
fi

# WAMR's AOT async termination uses shared memory plus thread-manager suspend
# checks. The import allowlist still prevents Guest thread APIs.
"$wasm_clangxx" \
    --target=wasm32-unknown-unknown \
    -std=c++23 -nostdlib -ffreestanding \
    -matomics -mbulk-memory \
    "${compile_profile_flags[@]}" \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics \
    -Wall -Wextra -Werror \
    "${guest_include_flags[@]}" \
    -Wl,--no-entry \
    -Wl,--shared-memory \
    -Wl,--max-memory=131072 \
    -Wl,--allow-undefined-file="$workspace_root/guest/abi/allowed_imports.txt" \
    -Wl,--export=__micropixel_start \
    -Wl,--export-if-defined=__wasm_call_ctors \
    -Wl,--export-if-defined=__micropixel_test_event_wait \
    -Wl,--export-if-defined=__micropixel_test_touch_pressure \
    ${link_profile_flags[@]+"${link_profile_flags[@]}"} \
    "$workspace_root/guest/runtime/startup.cpp" \
    "$workspace_root/guest/runtime/sdk.cpp" \
    "${source_paths[@]}" \
    -o "$output_dir/$app_name.wasm"

"$wamrc" \
    --target=riscv32 \
    --target-abi=ilp32f \
    --cpu=generic-rv32 \
    --cpu-features=+m,+a,+f,+c,+zicsr,+zifencei \
    --bounds-checks=1 \
    --enable-multi-thread \
    --enable-memory-profiling \
    --disable-ref-types \
    --disable-simd \
    "${aot_profile_flags[@]}" \
    -o "$output_dir/$app_name.aot" \
    "$output_dir/$app_name.wasm"

echo "P4 Guest built ($build_profile):"
ls -lh "$output_dir/$app_name".{wasm,aot}
