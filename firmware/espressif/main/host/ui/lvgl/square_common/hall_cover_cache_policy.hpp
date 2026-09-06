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
};

struct HallCoverCachePolicy final {
    static constexpr size_t kNoSlot = static_cast<size_t>(-1);

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
        for (size_t index = 0U; index < Capacity; ++index) {
            if (slots[index].app_index < window_first || slots[index].app_index >= window_last) {
                return index;
            }
        }
        return kNoSlot;
    }
};

}  // namespace micropixel::host_ui::lvgl::square_common

#endif
