#include <stddef.h>
#include <stdint.h>

extern "C" {

__attribute__((no_builtin("memcpy"))) void* memcpy(void* destination, const void* source, size_t length) {
    auto* output = static_cast<uint8_t*>(destination);
    const auto* input = static_cast<const uint8_t*>(source);
    for (size_t index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
    return destination;
}

__attribute__((no_builtin("memmove"))) void* memmove(void* destination, const void* source, size_t length) {
    auto* output = static_cast<uint8_t*>(destination);
    const auto* input = static_cast<const uint8_t*>(source);
    if (reinterpret_cast<uintptr_t>(output) < reinterpret_cast<uintptr_t>(input)) {
        for (size_t index = 0U; index < length; ++index) {
            output[index] = input[index];
        }
    } else if (reinterpret_cast<uintptr_t>(output) > reinterpret_cast<uintptr_t>(input)) {
        for (size_t index = length; index != 0U; --index) {
            output[index - 1U] = input[index - 1U];
        }
    }
    return destination;
}

__attribute__((no_builtin("memset"))) void* memset(void* destination, int value, size_t length) {
    auto* output = static_cast<uint8_t*>(destination);
    const uint8_t byte = static_cast<uint8_t>(value);
    for (size_t index = 0U; index < length; ++index) {
        output[index] = byte;
    }
    return destination;
}

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
