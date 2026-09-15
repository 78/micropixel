// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cstdint>
#include <cstdlib>

#include "platform/memory/internal_ram.hpp"

namespace {

uintptr_t internal_begin{};
uintptr_t internal_end{};

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

}  // namespace

bool micropixel_test_ptr_internal(const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    return address >= internal_begin && address < internal_end;
}

int main() {
    std::array<uint8_t, 64> state{};
    internal_begin = reinterpret_cast<uintptr_t>(&state);
    internal_end = internal_begin + sizeof(state);
    Require(micropixel::platform::memory::IsInternalObject(state));
    --internal_end;
    Require(!micropixel::platform::memory::IsInternalObject(state));
    ++internal_end;
    ++internal_begin;
    Require(!micropixel::platform::memory::IsInternalObject(state));
    internal_begin = 0U;
    internal_end = 0U;
    Require(!micropixel::platform::memory::IsInternalObject(state));
}
