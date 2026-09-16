#ifndef MICROPIXEL_GUEST_RUNTIME_DISPLAY_TRANSFORM_HPP
#define MICROPIXEL_GUEST_RUNTIME_DISPLAY_TRANSFORM_HPP

#include <stdint.h>

#include "sdk/display.hpp"

namespace micropixel::detail {

struct DisplayTransform final {
    uint32_t logical_width{};
    uint32_t logical_height{};
    uint32_t physical_width{};
    uint32_t physical_height{};
    uint32_t viewport_width{};
    uint32_t viewport_height{};
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

// Dimensions are bounded to keep coordinates and ABI rectangles representable.
[[nodiscard]] constexpr DisplayTransform MakeDisplayTransform(uint32_t screen_width, uint32_t screen_height,
                                                              DisplayConfiguration configuration = {}) {
    constexpr uint32_t kMaxExtent = 32767U;
    if (screen_width == 0U || screen_height == 0U || screen_width > kMaxExtent || screen_height > kMaxExtent) return {};
    DisplayTransform transform{
        .logical_width = screen_width,
        .logical_height = screen_height,
        .physical_width = screen_width,
        .physical_height = screen_height,
        .viewport_width = screen_width,
        .viewport_height = screen_height,
        .scale_numerator = 1U,
        .scale_denominator = 1U,
    };
    if (configuration.scale_mode == DisplayScaleMode::kNative) {
        if (configuration.logical_size.width != 0U || configuration.logical_size.height != 0U) return {};
        return transform;
    }
    if (configuration.scale_mode != DisplayScaleMode::kAspectFit &&
        configuration.scale_mode != DisplayScaleMode::kAspectFill &&
        configuration.scale_mode != DisplayScaleMode::kExpand)
        return {};
    const uint32_t width = configuration.logical_size.width;
    const uint32_t height = configuration.logical_size.height;
    if (width == 0U || height == 0U || width > kMaxExtent || height > kMaxExtent) return {};
    const bool width_limits =
        static_cast<uint64_t>(screen_width) * height <= static_cast<uint64_t>(screen_height) * width;
    const bool use_width = configuration.scale_mode == DisplayScaleMode::kAspectFill ? !width_limits : width_limits;
    transform.scale_numerator = use_width ? screen_width : screen_height;
    transform.scale_denominator = use_width ? width : height;
    const auto round_scale = [](uint32_t value, uint32_t n, uint32_t d) {
        return static_cast<uint32_t>((static_cast<uint64_t>(value) * n + d / 2U) / d);
    };
    transform.logical_width = width;
    transform.logical_height = height;
    if (configuration.scale_mode == DisplayScaleMode::kExpand) {
        transform.logical_width = round_scale(screen_width, transform.scale_denominator, transform.scale_numerator);
        transform.logical_height = round_scale(screen_height, transform.scale_denominator, transform.scale_numerator);
    } else {
        transform.viewport_width = round_scale(width, transform.scale_numerator, transform.scale_denominator);
        transform.viewport_height = round_scale(height, transform.scale_numerator, transform.scale_denominator);
        transform.offset_x = (static_cast<int32_t>(screen_width) - static_cast<int32_t>(transform.viewport_width)) / 2;
        transform.offset_y =
            (static_cast<int32_t>(screen_height) - static_cast<int32_t>(transform.viewport_height)) / 2;
    }
    if (transform.logical_width > kMaxExtent || transform.logical_height > kMaxExtent ||
        transform.viewport_width == 0U || transform.viewport_height == 0U || transform.viewport_width > kMaxExtent ||
        transform.viewport_height > kMaxExtent)
        return {};
    return transform;
}

[[nodiscard]] constexpr uint32_t ViewportWidth(const DisplayTransform& transform) {
    return transform.viewport_width ? transform.viewport_width : transform.physical_width;
}
[[nodiscard]] constexpr uint32_t ViewportHeight(const DisplayTransform& transform) {
    return transform.viewport_height ? transform.viewport_height : transform.physical_height;
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
    const auto inset = [](int64_t pixels, uint32_t logical, uint32_t physical) {
        if (pixels <= 0) return 0U;
        const auto value = ScaleInsetCeil(static_cast<uint32_t>(pixels), logical, physical);
        return value < logical ? value : logical;
    };
    return {
        .top =
            inset(static_cast<int64_t>(top) - transform.offset_y, transform.logical_height, ViewportHeight(transform)),
        .right = inset(
            static_cast<int64_t>(transform.offset_x) + ViewportWidth(transform) - transform.physical_width + right,
            transform.logical_width, ViewportWidth(transform)),
        .bottom = inset(
            static_cast<int64_t>(transform.offset_y) + ViewportHeight(transform) - transform.physical_height + bottom,
            transform.logical_height, ViewportHeight(transform)),
        .left =
            inset(static_cast<int64_t>(left) - transform.offset_x, transform.logical_width, ViewportWidth(transform)),
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
    return MapRect(x, y, width, height, transform.logical_width, transform.logical_height, ViewportWidth(transform),
                   ViewportHeight(transform), transform.offset_x, transform.offset_y);
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

// Map a source rectangle into an adaptively decoded texture. Independently
// rounded origins and extents preserve 1:1 copies for atlas frames, but two
// half-pixel round-ups can otherwise place the far edge one pixel past the
// decoded texture. Valid logical source rectangles are clamped to that edge.
[[nodiscard]] constexpr PhysicalRect MapTextureRect(int32_t x, int32_t y, int32_t width, int32_t height,
                                                    uint32_t logical_width, uint32_t logical_height,
                                                    uint32_t physical_width, uint32_t physical_height) {
    PhysicalRect mapped =
        MapSizedRect(x, y, width, height, logical_width, logical_height, physical_width, physical_height);
    const bool contained = x >= 0 && y >= 0 && width > 0 && height > 0 &&
                           static_cast<int64_t>(x) + width <= logical_width &&
                           static_cast<int64_t>(y) + height <= logical_height;
    if (!contained || physical_width == 0U || physical_height == 0U) {
        return mapped;
    }
    if (mapped.x >= static_cast<int32_t>(physical_width)) {
        mapped.x = static_cast<int32_t>(physical_width - 1U);
    }
    if (mapped.y >= static_cast<int32_t>(physical_height)) {
        mapped.y = static_cast<int32_t>(physical_height - 1U);
    }
    const int32_t available_width = static_cast<int32_t>(physical_width) - mapped.x;
    const int32_t available_height = static_cast<int32_t>(physical_height) - mapped.y;
    if (mapped.width > available_width) {
        mapped.width = available_width;
    }
    if (mapped.height > available_height) {
        mapped.height = available_height;
    }
    return mapped;
}

[[nodiscard]] constexpr PhysicalRect MapSceneSizedRect(const DisplayTransform& transform, int32_t x, int32_t y,
                                                       int32_t width, int32_t height) {
    return MapSizedRect(x, y, width, height, transform.logical_width, transform.logical_height,
                        ViewportWidth(transform), ViewportHeight(transform), transform.offset_x, transform.offset_y);
}

[[nodiscard]] constexpr int32_t MapSceneVectorX(const DisplayTransform& transform, int32_t value) {
    return ScaleCoordinate(value, ViewportWidth(transform), transform.logical_width);
}

[[nodiscard]] constexpr int32_t MapSceneVectorY(const DisplayTransform& transform, int32_t value) {
    return ScaleCoordinate(value, ViewportHeight(transform), transform.logical_height);
}

// The process-wide display context freezes on its first use.
[[nodiscard]] const DisplayTransform& CurrentDisplayTransform();

}  // namespace micropixel::detail

#endif
