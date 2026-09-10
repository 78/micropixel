#include "sdk/mode7_plane.hpp"

#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "sdk/graphics.hpp"

namespace micropixel {
namespace {

constexpr float kFixedOne = 65536.0F;  // 16.16

// Rounds toward negative infinity without libm (the SDK links none).
[[nodiscard]] int32_t FloorToInt(float value) {
    const auto truncated = static_cast<int32_t>(value);
    return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
}

[[nodiscard]] int32_t CeilToInt(float value) {
    const auto truncated = static_cast<int32_t>(value);
    return static_cast<float>(truncated) < value ? truncated + 1 : truncated;
}

}  // namespace

bool Mode7Plane::Initialize(const Mode7PlaneConfig& config) {
    valid_ = false;
    row_count_ = 0U;
    const Rect& view = config.viewport;
    if (view.x < 0 || view.y < 0 || view.width <= 0 || view.height <= 0 ||
        static_cast<uint32_t>(view.height) > kMaxRows || view.x > INT16_MAX - view.width ||
        view.y > INT16_MAX - view.height || !(config.camera_height > 0.0F) || !(config.focal_length > 0.0F) ||
        !(config.near_depth >= 0.0F) || !(config.far_depth > config.near_depth) || config.texture_width == 0U ||
        config.texture_height == 0U || config.light_levels == 0U ||
        config.light_levels > MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS ||
        (config.light_levels > 1U && !(config.shade_far > config.shade_near))) {
        return false;
    }
    config_ = config;
    centre_x_ = static_cast<float>(view.x) + static_cast<float>(view.width) * 0.5F;
    // Rows are sampled at their pixel centre; the first ground row is the one
    // whose centre lies below the horizon.
    const int32_t below = FloorToInt(config.horizon - 0.5F) + 1;
    const int32_t first = below > view.y ? below : view.y;
    for (int32_t y = first; y < view.y + view.height; ++y) {
        const float drop = (static_cast<float>(y) + 0.5F) - config.horizon;
        const float depth = config.camera_height * config.focal_length / drop;
        if (depth > config.far_depth) continue;
        if (depth < config.near_depth) break;
        Row& row = rows_[row_count_++];
        row.y = static_cast<int16_t>(y);
        row.depth = depth;
        row.scale = config.focal_length / depth;
        row.light = LightFor(depth);
        row.style = 0U;
        row.texture_slot = config.texture_slot;
        row.centre = centre_x_;
        row.half_width = 0.0F;
        row.visible = false;
    }
    valid_ = true;
    return true;
}

Mode7Plane::Projected Mode7Plane::Project(float depth, float lateral) const {
    Projected projected{};
    if (!valid_ || !(depth > 0.0F)) return projected;
    const float scale = config_.focal_length / depth;
    projected.x = centre_x_ + lateral * scale;
    projected.y = config_.horizon + config_.camera_height * scale;
    projected.scale = scale;
    projected.visible = depth >= config_.near_depth && depth <= config_.far_depth;
    return projected;
}

uint8_t Mode7Plane::LightFor(float depth) const {
    if (config_.light_levels <= 1U || depth <= config_.shade_near) return 0U;
    if (depth >= config_.shade_far) return static_cast<uint8_t>(config_.light_levels - 1U);
    const float fraction = (depth - config_.shade_near) / (config_.shade_far - config_.shade_near);
    const int32_t level = FloorToInt(fraction * static_cast<float>(config_.light_levels - 1U) + 0.5F);
    return static_cast<uint8_t>(level < 0 ? 0 : level);
}

bool Mode7Plane::RowExtent(const Row& row, int32_t& first, int32_t& last) const {
    if (!valid_ || !row.visible || !(row.half_width > 0.0F)) return false;
    const float left = row.centre - row.half_width;
    const float right = row.centre + row.half_width;
    const int32_t last_column = config_.viewport.x + config_.viewport.width - 1;
    if (right <= static_cast<float>(config_.viewport.x) || left >= static_cast<float>(last_column + 1) ||
        right - left < 0.5F) {
        return false;
    }
    // Pixels whose centre falls inside [left, right), clipped to the viewport.
    first = CeilToInt(left - 0.5F);
    if (first < config_.viewport.x) first = config_.viewport.x;
    last = FloorToInt(right - 0.5F);
    if (static_cast<float>(last) + 0.5F >= right) --last;
    if (last > last_column) last = last_column;
    return last >= first;
}

bool Mode7Plane::Draw(RasterDrawList& list) const {
    if (!valid_) return false;
    const float texture_rows = static_cast<float>(config_.texture_height);
    for (uint32_t index = 0U; index < row_count_; ++index) {
        const Row& row = rows_[index];
        int32_t first = 0;
        int32_t last = 0;
        if (row.style >= config_.texture_height || !RowExtent(row, first, last)) continue;
        const float left = row.centre - row.half_width;
        const float strip = row.half_width * 2.0F;
        // s runs 0..1 (16.16, texture width = 1.0) across the strip. The step
        // is rounded down so the accumulated s of the last pixel stays below
        // 1.0 and never wraps to texel 0.
        const float texels_per_pixel = kFixedOne / strip;
        const auto ds = static_cast<int32_t>(texels_per_pixel);
        const auto s = static_cast<int32_t>((static_cast<float>(first) + 0.5F - left) * texels_per_pixel);
        const auto t = static_cast<int32_t>((static_cast<float>(row.style) + 0.5F) / texture_rows * kFixedOne);
        if (ds <= 0) continue;
        if (!list.Span(static_cast<uint16_t>(row.y), static_cast<uint16_t>(first), static_cast<uint16_t>(last),
                       row.texture_slot, row.light, s, t, ds, 0)) {
            return false;
        }
    }
    return true;
}

}  // namespace micropixel
