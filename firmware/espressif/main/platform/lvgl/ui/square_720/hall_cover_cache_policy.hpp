#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_HALL_COVER_CACHE_POLICY_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_HALL_COVER_CACHE_POLICY_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace micropixel::platform::lvgl::square_720 {

struct HallCoverCacheSlot final {
    bool occupied{};
    uint64_t key{};
    uint32_t app_index{};
};

struct HallCoverCachePolicy final {
    static constexpr size_t kNoSlot = static_cast<size_t>(-1);

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

}  // namespace micropixel::platform::lvgl::square_720

#endif
