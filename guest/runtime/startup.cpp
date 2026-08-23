#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "runtime/panic.hpp"

extern int main();
extern "C" void __wasm_call_ctors(void);

namespace {

bool HostAbiIsCompatible() {
    uint32_t version = micropixel_abi_version();
    uint32_t major = version >> 16U;
    uint32_t minor = version & 0xffffU;
    return major == MICROPIXEL_ABI_VERSION_MAJOR && minor >= MICROPIXEL_ABI_VERSION_MINOR;
}

}  // namespace

extern "C" __attribute__((export_name("__micropixel_start"))) int32_t __micropixel_start(void) {
    if (!HostAbiIsCompatible()) {
        micropixel::runtime::Panic("startup.abi_version", MICROPIXEL_STATUS_UNSUPPORTED);
    }

    static bool initialized = false;
    if (!initialized) {
        __wasm_call_ctors();
        initialized = true;
    }
    return static_cast<int32_t>(main());
}
