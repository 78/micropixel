#include <stddef.h>
#include <stdint.h>

extern "C" {

// Guests are always compiled with -mbulk-memory (tools/micropixel), so the
// builtins below lower to single memory.copy / memory.fill instructions that
// the Host AOT turns into its native block copy. With -ffreestanding the
// compiler never rewrites a call to memcpy() into the builtin on its own,
// which is why these definitions have to spell it out; they cannot recurse
// because the builtin is an instruction here, not a call.
#if !defined(__wasm_bulk_memory__)
#error "MicroPixel Guests require -mbulk-memory"
#endif

void* memcpy(void* destination, const void* source, size_t length) {
    return __builtin_memcpy(destination, source, length);
}

void* memmove(void* destination, const void* source, size_t length) {
    return __builtin_memmove(destination, source, length);
}

void* memset(void* destination, int value, size_t length) { return __builtin_memset(destination, value, length); }

__attribute__((no_builtin("memcmp"))) int memcmp(const void* left, const void* right, size_t length) {
    const auto* left_bytes = static_cast<const uint8_t*>(left);
    const auto* right_bytes = static_cast<const uint8_t*>(right);
    for (size_t index = 0U; index < length; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return left_bytes[index] < right_bytes[index] ? -1 : 1;
        }
    }
    return 0;
}

__attribute__((no_builtin("memchr"))) void* memchr(const void* memory, int value, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(memory);
    const uint8_t expected = static_cast<uint8_t>(value);
    for (size_t index = 0U; index < length; ++index) {
        if (bytes[index] == expected) {
            return const_cast<uint8_t*>(bytes + index);
        }
    }
    return nullptr;
}

__attribute__((no_builtin("strlen"))) size_t strlen(const char* text) {
    size_t length = 0U;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

}  // extern "C"
