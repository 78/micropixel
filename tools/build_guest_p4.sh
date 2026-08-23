#!/usr/bin/env bash
set -euo pipefail

workspace_root="$(cd "$(dirname "$0")/.." && pwd)"
output_dir="${P4_GUEST_OUTPUT_DIR:-$workspace_root/build/guest-p4}"

if [[ -n "${WASI_CLANG:-}" ]]; then
    wasm_clang="$WASI_CLANG"
elif [[ -n "${WASI_SDK_PATH:-}" ]]; then
    wasm_clang="$WASI_SDK_PATH/bin/clang"
elif [[ -n "${HOME:-}" && -x "$HOME/.local/wasi-sdk/bin/clang" ]]; then
    wasm_clang="$HOME/.local/wasi-sdk/bin/clang"
else
    echo "Set WASI_CLANG or WASI_SDK_PATH to a Clang with the wasm32 backend." >&2
    exit 2
fi

wamrc="${WAMRC:-$workspace_root/build/tools/wamrc-2.4.5-riscv32-ilp32f}"
if [[ ! -x "$wasm_clang" ]]; then
    echo "WASI Clang is not executable: $wasm_clang" >&2
    exit 2
fi
if [[ ! -x "$wamrc" ]]; then
    echo "Set WAMRC to a WAMR 2.4.5 compiler built for RISCV32_ILP32F." >&2
    exit 2
fi

mkdir -p "$output_dir"

compile_aot() {
    local name="$1"

    "$wamrc" \
        --target=riscv32 \
        --target-abi=ilp32f \
        --cpu=generic-rv32 \
        --cpu-features=+m,+a,+f,+c,+zicsr,+zifencei \
        --bounds-checks=1 \
        --enable-memory-profiling \
        --disable-ref-types \
        --disable-simd \
        -o "$output_dir/$name.aot" \
        "$output_dir/$name.wasm"

}

build_bad_import() {
    "$wasm_clang" \
        --target=wasm32-unknown-unknown \
        -Oz -nostdlib \
        -Wl,--no-entry \
        -Wl,--allow-undefined \
        -Wl,--export=__micropixel_start \
        -Wl,--strip-all \
        "$workspace_root/guest/tests/bad_import.c" \
        -o "$output_dir/bad_import.wasm"
    compile_aot bad_import
}

build_sdk_example() {
    local name="$1"
    local source="$2"
    P4_GUEST_OUTPUT_DIR="$output_dir" \
        bash "$workspace_root/tools/build_guest_app_p4.sh" "$source" "$name"
}

build_bad_import
build_sdk_example sdk_hello "$workspace_root/guest/tests/conformance/sdk_hello.cpp"
build_sdk_example event_wait "$workspace_root/guest/tests/conformance/event_wait.cpp"
build_sdk_example graphics_protocol "$workspace_root/guest/tests/conformance/graphics_protocol.cpp"
build_sdk_example graphics_invalid_pointer "$workspace_root/guest/tests/conformance/graphics_invalid_pointer.cpp"
build_sdk_example touch_pressure "$workspace_root/guest/tests/conformance/touch_pressure.cpp"
build_sdk_example timer_counter "$workspace_root/guest/tests/conformance/timer_counter.cpp"
build_sdk_example schedule_after "$workspace_root/guest/tests/conformance/schedule_after.cpp"
build_sdk_example schedule_multiple "$workspace_root/guest/tests/conformance/schedule_multiple.cpp"
build_sdk_example main_failure "$workspace_root/guest/tests/conformance/main_failure.cpp"
build_sdk_example watchdog_spin "$workspace_root/guest/tests/conformance/watchdog_spin.cpp"
build_sdk_example sdk_panic "$workspace_root/guest/tests/conformance/sdk_panic.cpp"
build_sdk_example application_assert "$workspace_root/guest/tests/conformance/application_assert.cpp"
build_sdk_example audio_synth "$workspace_root/guest/tests/conformance/audio_synth.cpp"
build_sdk_example service_control "$workspace_root/guest/tests/conformance/service_control.cpp"

built_guests=(
    bad_import sdk_hello event_wait graphics_protocol graphics_invalid_pointer
    touch_pressure timer_counter
    schedule_after schedule_multiple main_failure watchdog_spin sdk_panic application_assert audio_synth service_control
)
for guest_name in "${built_guests[@]}"; do
    ls -lh "$output_dir/$guest_name".{wasm,aot}
done
