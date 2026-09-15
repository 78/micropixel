// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <limits>

#include "runtime/wamr/linear_memory_policy.hpp"

int main() {
    using namespace micropixel::runtime;
    constexpr size_t kMiB = 1024U * 1024U;
    constexpr size_t kReserve = 2U * kMiB;

    // Fragmented S3 heap: the reserve need not fit alongside the Guest in the
    // largest block. The module's 17 initial pages fit within this budget.
    assert(GuestLinearMemoryPageLimit({4090440U, 2752512U}, kReserve, 128U) == 30U);
    assert(GuestLinearMemoryPageLimit({6U * kMiB, kMiB + 64U}, kReserve, 128U) == 16U);
    assert(GuestLinearMemoryPageLimit({3U * kMiB + 64U, 3U * kMiB}, kReserve, 128U) == 16U);
    assert(GuestLinearMemoryPageLimit({32U * kMiB, 32U * kMiB}, kReserve, 128U) == 128U);

    assert(GuestLinearMemoryPageLimit({kReserve - 1U, kMiB}, kReserve, 128U) == 0U);
    assert(GuestLinearMemoryPageLimit({kReserve, kMiB}, kReserve, 128U) == 0U);
    assert(GuestLinearMemoryPageLimit({kReserve + kWasmPageBytes + 63U, kMiB}, kReserve, 128U) == 0U);
    assert(GuestLinearMemoryPageLimit({kReserve + kWasmPageBytes + 64U, kMiB}, kReserve, 128U) == 1U);
    assert(GuestLinearMemoryPageLimit({8U * kMiB, 64U}, kReserve, 128U) == 0U);
    assert(GuestLinearMemoryPageLimit({8U * kMiB, kWasmPageBytes + 63U}, kReserve, 128U) == 0U);
    assert(GuestLinearMemoryPageLimit({8U * kMiB, kWasmPageBytes + 64U}, kReserve, 128U) == 1U);
    assert(GuestLinearMemoryPageLimit({8U * kMiB, 8U * kMiB}, kReserve, 0U) == 0U);
    assert(GuestLinearMemoryPageLimit({std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()},
                                      kReserve, 128U) == 128U);

    // Every advertised limit must satisfy both existing allocation guards.
    for (size_t free = 0U; free <= 12U * kMiB; free += kWasmPageBytes / 2U) {
        for (size_t largest = 0U; largest <= free; largest += kWasmPageBytes / 2U) {
            const GuestPsramState state{free, largest};
            const uint32_t pages = GuestLinearMemoryPageLimit(state, kReserve, 128U);
            if (pages != 0U) {
                assert(IsGuestPsramAllocationAdmissible(
                    state, pages * kWasmPageBytes + kGuestLinearMemoryAllocationOverhead, kReserve));
            }
        }
    }
}
