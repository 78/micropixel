#ifndef MICROPIXEL_RUNTIME_PANIC_HPP
#define MICROPIXEL_RUNTIME_PANIC_HPP

#include <stdint.h>

namespace micropixel::runtime {

[[noreturn]] void Panic(const char* operation, int32_t status);

}  // namespace micropixel::runtime

#endif
