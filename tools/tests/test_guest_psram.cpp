#include <cassert>
#include <cstddef>

#include "runtime/memory/guest_psram.hpp"

int main() {
    using micropixel::runtime::GuestPsramState;
    using micropixel::runtime::IsGuestPsramAllocationAdmissible;
    using micropixel::runtime::IsGuestPsramGrowthAdmissible;

    constexpr size_t kMiB = 1024U * 1024U;
    static_assert(IsGuestPsramAllocationAdmissible({12U * kMiB, 11U * kMiB}, 8U * kMiB, 2U * kMiB));
    static_assert(!IsGuestPsramAllocationAdmissible({12U * kMiB, 7U * kMiB}, 8U * kMiB, 2U * kMiB));
    static_assert(!IsGuestPsramAllocationAdmissible({9U * kMiB, 9U * kMiB}, 8U * kMiB, 2U * kMiB));
    static_assert(!IsGuestPsramAllocationAdmissible({12U * kMiB, 11U * kMiB}, 0U, 2U * kMiB));
    static_assert(IsGuestPsramGrowthAdmissible(6U * kMiB, 4U * kMiB, 2U * kMiB));
    static_assert(!IsGuestPsramGrowthAdmissible(6U * kMiB, 4U * kMiB + 1U, 2U * kMiB));

    const GuestPsramState fragmented{.free_bytes = 10U * kMiB, .largest_free_block = 3U * kMiB};
    assert(!IsGuestPsramAllocationAdmissible(fragmented, 4U * kMiB, 2U * kMiB));
    return 0;
}
