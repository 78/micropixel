#ifndef MICROPIXEL_HOST_UI_LVGL_SQUARE_COMMON_HALL_COVER_CACHE_POLICY_HPP
#define MICROPIXEL_HOST_UI_LVGL_SQUARE_COMMON_HALL_COVER_CACHE_POLICY_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// Nonzero keys identify immutable bundle assets. Unkeyed RAM snapshots are
// valid only for one catalog update, even if the allocator reuses an address.
struct HallCoverCacheIdentity final {
    uint64_t key{};
    const uint8_t* source{};
    uint64_t catalog_generation{};

    [[nodiscard]] constexpr bool Matches(const HallCoverCacheIdentity& other) const {
        return key == other.key &&
               (key != 0U || (source == other.source && catalog_generation == other.catalog_generation));
    }
};

struct HallCoverCacheSlot final {
    bool occupied{};
    uint64_t key{};
    uint32_t app_index{};
    uint64_t last_used{};
};

struct HallCoverCachePolicy final {
    static constexpr size_t kNoSlot = static_cast<size_t>(-1);

    static constexpr size_t kCapacity = host_ui::kMaxHallApps;
    static constexpr size_t kPsramReserve = 2U * 1024U * 1024U;

    [[nodiscard]] static constexpr bool RetainForLaunch(uint32_t app_index, uint32_t window_first, uint32_t window_last,
                                                        bool launch_image) {
        return launch_image || (app_index >= window_first && app_index < window_last);
    }

    [[nodiscard]] static constexpr bool CanGrow(size_t free_bytes, size_t allocation_bytes) {
        return free_bytes >= allocation_bytes && free_bytes - allocation_bytes >= kPsramReserve;
    }

    template <size_t Capacity>
    [[nodiscard]] static constexpr size_t OldestOutsideWindow(const std::array<HallCoverCacheSlot, Capacity>& slots,
                                                              uint32_t first, uint32_t last) {
        size_t oldest = kNoSlot;
        for (size_t index = 0U; index < Capacity; ++index) {
            if (slots[index].occupied && (slots[index].app_index < first || slots[index].app_index >= last) &&
                (oldest == kNoSlot || slots[index].last_used < slots[oldest].last_used)) {
                oldest = index;
            }
        }
        return oldest;
    }

    // Retained PPA screenshots are tightly packed; decoded cache entries use
    // LVGL's padded rows. An image descriptor supports either explicit stride.
    [[nodiscard]] static constexpr bool CanUseSourceDirectly(const host_ui::HallCoverModel& source,
                                                             uint32_t target_size) {
        return source.data != nullptr && target_size != 0U && source.format == host_ui::HallCoverFormat::kRgb888 &&
               source.width == target_size && source.height == target_size &&
               source.stride >= static_cast<uint64_t>(target_size) * 3U &&
               source.size >= static_cast<uint64_t>(source.stride) * target_size;
    }

    template <size_t Capacity>
    [[nodiscard]] static constexpr size_t ReplacementIndex(const std::array<HallCoverCacheSlot, Capacity>& slots,
                                                           uint64_t requested_key, uint32_t requested_app_index,
                                                           uint32_t window_first, uint32_t window_last) {
        for (size_t index = 0U; index < Capacity; ++index) {
            if (!slots[index].occupied) {
                return index;
            }
        }
        for (size_t index = 0U; index < Capacity; ++index) {
            if (slots[index].app_index == requested_app_index && slots[index].key != requested_key) {
                return index;
            }
        }
        return OldestOutsideWindow(slots, window_first, window_last);
    }
};

}  // namespace micropixel::host_ui::lvgl::square_common

#endif
