#ifndef MICROPIXEL_GUEST_RUNTIME_DISPLAY_TRANSFORM_HPP
#define MICROPIXEL_GUEST_RUNTIME_DISPLAY_TRANSFORM_HPP

#include <stdint.h>

namespace micropixel::detail {

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

struct PhysicalRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

struct LogicalInsets final {
    uint32_t top{};
    uint32_t right{};
    uint32_t bottom{};
    uint32_t left{};
};

[[nodiscard]] constexpr DisplayTransform MakeDisplayTransform(uint32_t screen_width, uint32_t screen_height) {
    constexpr uint32_t kLogicalShortEdge = 720U;
    if (screen_width == 0U || screen_height == 0U) {
        return {};
    }
    DisplayTransform transform{
        .physical_width = screen_width,
        .physical_height = screen_height,
        .scale_numerator = screen_width < screen_height ? screen_width : screen_height,
        .scale_denominator = kLogicalShortEdge,
    };
    if (screen_width <= screen_height) {
        transform.logical_width = kLogicalShortEdge;
        transform.logical_height = static_cast<uint32_t>(
            (static_cast<uint64_t>(screen_height) * kLogicalShortEdge + screen_width / 2U) / screen_width);
    } else {
        transform.logical_height = kLogicalShortEdge;
        transform.logical_width = static_cast<uint32_t>(
            (static_cast<uint64_t>(screen_width) * kLogicalShortEdge + screen_height / 2U) / screen_height);
    }
    return transform;
}

[[nodiscard]] constexpr int32_t ScaleCoordinate(int32_t value, uint32_t numerator, uint32_t denominator) {
    if (denominator == 0U) {
        return 0;
    }
    const int64_t product = static_cast<int64_t>(value) * numerator;
    const int64_t rounding = denominator / 2U;
    return static_cast<int32_t>(product >= 0 ? (product + rounding) / denominator : (product - rounding) / denominator);
}

[[nodiscard]] constexpr uint32_t ScaleInsetCeil(uint32_t value, uint32_t logical_extent, uint32_t physical_extent) {
    if (physical_extent == 0U) {
        return 0U;
    }
    return static_cast<uint32_t>((static_cast<uint64_t>(value) * logical_extent + physical_extent - 1U) /
                                 physical_extent);
}

[[nodiscard]] constexpr LogicalInsets MapPhysicalInsets(const DisplayTransform& transform, uint32_t top, uint32_t right,
                                                        uint32_t bottom, uint32_t left) {
    return {
        .top = ScaleInsetCeil(top, transform.logical_height, transform.physical_height),
        .right = ScaleInsetCeil(right, transform.logical_width, transform.physical_width),
        .bottom = ScaleInsetCeil(bottom, transform.logical_height, transform.physical_height),
        .left = ScaleInsetCeil(left, transform.logical_width, transform.physical_width),
    };
}

[[nodiscard]] constexpr PhysicalRect MapRect(int32_t x, int32_t y, int32_t width, int32_t height,
                                             uint32_t logical_width, uint32_t logical_height, uint32_t physical_width,
                                             uint32_t physical_height, int32_t offset_x = 0, int32_t offset_y = 0) {
    const int32_t left = offset_x + ScaleCoordinate(x, physical_width, logical_width);
    const int32_t top = offset_y + ScaleCoordinate(y, physical_height, logical_height);
    int32_t right = offset_x + ScaleCoordinate(x + width, physical_width, logical_width);
    int32_t bottom = offset_y + ScaleCoordinate(y + height, physical_height, logical_height);
    if (width > 0 && right <= left) {
        right = left + 1;
    }
    if (height > 0 && bottom <= top) {
        bottom = top + 1;
    }
    return {.x = left, .y = top, .width = right - left, .height = bottom - top};
}

[[nodiscard]] constexpr PhysicalRect MapSceneRect(const DisplayTransform& transform, int32_t x, int32_t y,
                                                  int32_t width, int32_t height) {
    return MapRect(x, y, width, height, transform.logical_width, transform.logical_height, transform.physical_width,
                   transform.physical_height, transform.offset_x, transform.offset_y);
}

// Textures are adaptively decoded with independently rounded physical width
// and height. Preserve those extents here so a logical 1:1 source/destination
// remains a physical 1:1 hardware copy regardless of its screen phase.
[[nodiscard]] constexpr PhysicalRect MapSizedRect(int32_t x, int32_t y, int32_t width, int32_t height,
                                                  uint32_t logical_width, uint32_t logical_height,
                                                  uint32_t physical_width, uint32_t physical_height,
                                                  int32_t offset_x = 0, int32_t offset_y = 0) {
    const int32_t mapped_width = ScaleCoordinate(width, physical_width, logical_width);
    const int32_t mapped_height = ScaleCoordinate(height, physical_height, logical_height);
    return {.x = offset_x + ScaleCoordinate(x, physical_width, logical_width),
            .y = offset_y + ScaleCoordinate(y, physical_height, logical_height),
            .width = width > 0 && mapped_width <= 0 ? 1 : mapped_width,
            .height = height > 0 && mapped_height <= 0 ? 1 : mapped_height};
}

[[nodiscard]] constexpr PhysicalRect MapSceneSizedRect(const DisplayTransform& transform, int32_t x, int32_t y,
                                                       int32_t width, int32_t height) {
    return MapSizedRect(x, y, width, height, transform.logical_width, transform.logical_height,
                        transform.physical_width, transform.physical_height, transform.offset_x, transform.offset_y);
}

[[nodiscard]] constexpr int32_t MapSceneVectorX(const DisplayTransform& transform, int32_t value) {
    return ScaleCoordinate(value, transform.physical_width, transform.logical_width);
}

[[nodiscard]] constexpr int32_t MapSceneVectorY(const DisplayTransform& transform, int32_t value) {
    return ScaleCoordinate(value, transform.physical_height, transform.logical_height);
}

// System font handles are ordered small..title. The physical Host font is
// selected here so MeasureText() and retained Label serialization use the same
// logical metrics on lower-density displays.
[[nodiscard]] constexpr uint16_t MapSystemFont(uint16_t font, const DisplayTransform& transform) {
    return transform.scale_numerator < transform.scale_denominator && font > 1U && font <= 4U ? font - 1U : font;
}

// The process-wide display context is initialized by Application and remains
// immutable for the lifetime of a Guest instance.
[[nodiscard]] const DisplayTransform& CurrentDisplayTransform();

}  // namespace micropixel::detail

#endif
