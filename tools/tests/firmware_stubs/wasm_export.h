#ifndef MICROPIXEL_TEST_STUB_WASM_EXPORT_H
#define MICROPIXEL_TEST_STUB_WASM_EXPORT_H

#include <stdbool.h>
#include <stdint.h>

static inline bool wasm_runtime_is_xip_file(const uint8_t* bytes, uint32_t size) {
    (void)bytes;
    (void)size;
    return false;
}

#endif
