// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_RUNTIME_WAMR_LINEAR_MEMORY_POLICY_HPP
#define MICROPIXEL_RUNTIME_WAMR_LINEAR_MEMORY_POLICY_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "runtime/memory/guest_psram.hpp"

namespace micropixel::runtime {

inline constexpr size_t kWasmPageBytes = 64U * 1024U;
inline constexpr size_t kGuestLinearMemoryAllocationOverhead = 64U;

[[nodiscard]] constexpr uint32_t GuestLinearMemoryPageLimit(const GuestPsramState& state, size_t reserve_bytes,
                                                            uint32_t configured_max_pages) {
    // The Host reserve can occupy other free blocks; only the Guest allocation
    // itself must fit in one contiguous block.
    const size_t available_bytes = state.free_bytes > reserve_bytes ? state.free_bytes - reserve_bytes : 0U;
    const size_t allocation_bytes = std::min(state.largest_free_block, available_bytes);
    const size_t pages = allocation_bytes > kGuestLinearMemoryAllocationOverhead
                             ? (allocation_bytes - kGuestLinearMemoryAllocationOverhead) / kWasmPageBytes
                             : 0U;
    return static_cast<uint32_t>(std::min(pages, static_cast<size_t>(configured_max_pages)));
}

}  // namespace micropixel::runtime

#endif
