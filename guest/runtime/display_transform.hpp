#ifndef MICROPIXEL_GUEST_RUNTIME_DISPLAY_TRANSFORM_HPP
#define MICROPIXEL_GUEST_RUNTIME_DISPLAY_TRANSFORM_HPP

#include <stdint.h>

namespace micropixel::detail {

enum class AppDisplayProfile : uint32_t {
    kSquare = 1U,
    kLandscape = 2U,
    kPortrait = 3U,
};

#ifndef MICROPIXEL_APP_DISPLAY_PROFILE
#define MICROPIXEL_APP_DISPLAY_PROFILE 1
#endif

static_assert(MICROPIXEL_APP_DISPLAY_PROFILE >= 1 && MICROPIXEL_APP_DISPLAY_PROFILE <= 3,
              "MICROPIXEL_APP_DISPLAY_PROFILE must be square, landscape, or portrait");

inline constexpr AppDisplayProfile kConfiguredDisplayProfile =
    static_cast<AppDisplayProfile>(MICROPIXEL_APP_DISPLAY_PROFILE);

struct DisplayTransform final {
    uint32_t logical_width{};
    uint32_t logical_height{};
    uint32_t physical_width{};
    uint32_t physical_height{};
    int32_t offset_x{};
    int32_t offset_y{};
    uint32_t scale_numerator{};
    uint32_t scale_denominator{};
};

[[nodiscard]] constexpr DisplayTransform MakeDisplayTransform(AppDisplayProfile profile, uint32_t screen_width,
                                                              uint32_t screen_height) {
    constexpr uint32_t kLogicalShortEdge = 720U;
    if (screen_width == 0U || screen_height == 0U) {
        return {};
    }
    DisplayTransform transform{};
    transform.scale_denominator = kLogicalShortEdge;
    switch (profile) {
        case AppDisplayProfile::kSquare: {
            const uint32_t edge = screen_width < screen_height ? screen_width : screen_height;
            transform.logical_width = kLogicalShortEdge;
            transform.logical_height = kLogicalShortEdge;
            transform.physical_width = edge;
            transform.physical_height = edge;
            transform.offset_x = static_cast<int32_t>((screen_width - edge) / 2U);
            transform.offset_y = static_cast<int32_t>((screen_height - edge) / 2U);
            transform.scale_numerator = edge;
            break;
        }
        case AppDisplayProfile::kLandscape:
            if (screen_width <= screen_height) {
                return {};
            }
            transform.logical_height = kLogicalShortEdge;
            transform.logical_width = static_cast<uint32_t>(
                (static_cast<uint64_t>(screen_width) * kLogicalShortEdge + screen_height / 2U) / screen_height);
            transform.physical_width = screen_width;
            transform.physical_height = screen_height;
            transform.scale_numerator = screen_height;
            break;
        case AppDisplayProfile::kPortrait:
            if (screen_height <= screen_width) {
                return {};
            }
            transform.logical_width = kLogicalShortEdge;
            transform.logical_height = static_cast<uint32_t>(
                (static_cast<uint64_t>(screen_height) * kLogicalShortEdge + screen_width / 2U) / screen_width);
            transform.physical_width = screen_width;
            transform.physical_height = screen_height;
            transform.scale_numerator = screen_width;
            break;
    }
    return transform;
}

[[nodiscard]] constexpr DisplayTransform MakeDisplayTransform(uint32_t screen_width, uint32_t screen_height) {
    return MakeDisplayTransform(kConfiguredDisplayProfile, screen_width, screen_height);
}

[[nodiscard]] constexpr int32_t ScaleCoordinate(int32_t value, uint32_t numerator, uint32_t denominator) {
    if (denominator == 0U) {
        return 0;
    }
    const int64_t product = static_cast<int64_t>(value) * numerator;
    const int64_t rounding = denominator / 2U;
    return static_cast<int32_t>(product >= 0 ? (product + rounding) / denominator : (product - rounding) / denominator);
}

}  // namespace micropixel::detail

#endif
