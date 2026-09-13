// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <mutex>
#include <span>

#include "device/contracts/block_storage.hpp"
#include "platform/memory/psram_buffer.hpp"

namespace micropixel::platform::storage {

// Shares identical whole-file page sequences. No partial windows or merging. The owner must
// outlive every lease. All operations serialize; this is not a drawing/ISR API.
class FlashPageMappingCache final {
   public:
    static constexpr size_t kWindowCapacity = 64U;
    static constexpr size_t kLeaseCapacity = 256U;
    static constexpr size_t kMaxPages = 1024U;

    FlashPageMappingCache() = default;
    ~FlashPageMappingCache();
    FlashPageMappingCache(const FlashPageMappingCache&) = delete;
    FlashPageMappingCache& operator=(const FlashPageMappingCache&) = delete;

    // pages must be suitable for spi_flash_mmap_pages (internal RAM on ESP-IDF).
    [[nodiscard]] std::expected<device::BlockStorageMapping, device::BlockStorageError> Map(std::span<const int> pages);
    void Unmap(device::BlockStorageMapping& mapping);

   private:
    struct Window {
        const void* data{};
        int* pages{};  // Owned PSRAM metadata, released with the backend window.
        size_t page_count{};
        uint32_t backend_handle{};
        uint32_t references{};
    };
    struct Lease {
        const void* data{};
        uint32_t handle{};
        size_t window_index{};
    };
    struct State {
        std::array<Window, kWindowCapacity> windows{};
        std::array<Lease, kLeaseCapacity> leases{};
    };
    static void ReleaseWindow(Window& window);

    std::mutex mutex_;
    memory::PsramBuffer<State> state_;
    uint32_t next_handle_{1U};  // Zero means exhausted; never recycle stale handles.
};

}  // namespace micropixel::platform::storage
