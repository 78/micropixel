#ifndef MICROPIXEL_SDK_MODE7_PLANE_HPP
#define MICROPIXEL_SDK_MODE7_PLANE_HPP

#include <stdint.h>

#include "sdk/geometry.hpp"

namespace micropixel {

class RasterDrawList;

// A textured ground plane seen in perspective from a camera above it, drawn
// with one RasterDrawList::Span record per screen row (Graphics 1.6): the
// "Mode 7" of pseudo-3D racers, kart tracks and top-down ground.
//
// Every screen row below the horizon sees the plane at one depth, so the
// plane's texture crosses that row at one scale. Mode7Plane precomputes the
// depth, pixel scale and light level of every row once; each frame the App
// places the textured strip on each row (its centre and half width in buffer
// pixels, following the track's curvature, and which texture row it shows)
// and Draw() clips the rows to the buffer and emits the Span records with
// their 16.16 steps. Objects on the plane (karts, signs, boost pads) are
// placed with Project() and drawn by the App as Image/Sprite records, sorted
// far to near.
//
// View space: x right, y down, depth away from the camera. A point `depth`
// world units ahead and `lateral` units right of the camera lands at
//   x = viewport centre + lateral * focal_length / depth
//   y = horizon + camera_height * focal_length / depth
// with focal_length in buffer pixels per world unit at depth 1.
struct Mode7PlaneConfig final {
    // Buffer area the plane is drawn in (the whole HostSurface buffer, or the
    // game's square inside a letterboxed one): rows outside it are never
    // created and every Span is clipped to it. Lateral 0 projects onto its
    // horizontal centre.
    Rect viewport{};
    // Buffer y of the horizon, where depth is infinite; rows below it show
    // the plane. May be fractional (a design-unit horizon scaled to the
    // buffer) and may lie outside the viewport.
    float horizon{};
    float camera_height{1.0F};
    float focal_length{1.0F};
    // Rows whose depth is outside [near_depth, far_depth] are never drawn
    // (too close to the camera, or too far to matter).
    float near_depth{0.0F};
    float far_depth{1.0e9F};
    // kRowMajor INDEX8 strip texture: each texture row is one strip style
    // (road with a dash, finish line, grass edge...) running across the plane.
    // A power-of-two width samples fastest.
    uint8_t texture_slot{};
    uint16_t texture_width{};
    uint16_t texture_height{};
    // Depth shading through the lit palette the Span records use: rows at or
    // before shade_near take light 0, rows at or beyond shade_far take
    // light_levels - 1, linearly in between. light_levels 1 disables it.
    uint8_t light_levels{1};
    float shade_near{0.0F};
    float shade_far{1.0F};
};

class Mode7Plane final {
   public:
    // One screen row of the plane. y, depth, scale and light are fixed by
    // Initialize(); the App sets the strip placement and style every frame.
    struct Row final {
        int16_t y{};
        uint8_t light{};
        // Texture row sampled by this screen row (0..texture_height - 1).
        uint8_t style{};
        // Texture sampled by this row; Initialize() sets config.texture_slot.
        // An App with the strip at several widths (mip levels) may pick one
        // per row so far rows do not skip texels; all share texture_height.
        uint8_t texture_slot{};
        // World distance ahead of the camera, and buffer pixels per world unit
        // at that distance (focal_length / depth).
        float depth{};
        float scale{};
        // Strip placement in buffer pixels: texture columns 0..texture_width
        // are stretched over [centre - half_width, centre + half_width).
        float centre{};
        float half_width{};
        // False hides the row this frame (for example outside the track).
        bool visible{};
    };

    struct Projected final {
        float x{};
        float y{};
        // Buffer pixels per world unit at this depth: multiply object sizes by it.
        float scale{};
        // False when the point is behind the camera or outside the depth range.
        bool visible{};
    };

    // Rows the plane can hold: a full-height buffer without upscale.
    static constexpr uint32_t kMaxRows = 768U;

    // False (and the plane stays unusable) when the viewport is empty, has a
    // negative origin or is taller than kMaxRows, when camera_height or
    // focal_length is not positive, the depth
    // range is empty, a texture dimension is 0, or light_levels is 0 or above
    // the palette limit (32).
    [[nodiscard]] bool Initialize(const Mode7PlaneConfig& config);
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] const Mode7PlaneConfig& config() const { return config_; }

    // Rows from the horizon down to the bottom of the viewport, within the depth
    // range; farthest first, nearest last. Empty before Initialize().
    [[nodiscard]] uint32_t row_count() const { return row_count_; }
    [[nodiscard]] Row* rows() { return rows_; }
    [[nodiscard]] const Row* rows() const { return rows_; }

    [[nodiscard]] Projected Project(float depth, float lateral) const;
    [[nodiscard]] uint8_t LightFor(float depth) const;

    // Places every row's strip from its depth: centre = viewport centre +
    // (centre_offset(depth) - camera_x) * scale and half_width = strip_half *
    // scale, where `centre_offset` returns the plane's lateral offset at a
    // depth (curvature accumulated by the track model). Rows keep their
    // style. Convenience over writing rows() directly.
    template <typename CentreOffset>
    void PlaceRows(CentreOffset&& centre_offset, float camera_x, float strip_half) {
        for (uint32_t index = 0U; index < row_count_; ++index) {
            Row& row = rows_[index];
            row.centre = centre_x_ + (centre_offset(row.depth) - camera_x) * row.scale;
            row.half_width = strip_half * row.scale;
            row.visible = true;
        }
    }

    // Buffer columns first..last (inclusive) that `row` covers after clipping
    // to the viewport: the pixels whose centre lies inside the strip. False
    // when the row is hidden or covers no pixel. Lets the App draw only the
    // parts of a backdrop the plane leaves visible.
    [[nodiscard]] bool RowExtent(const Row& row, int32_t& first, int32_t& last) const;

    // One Span per visible row whose strip reaches the viewport, far to near.
    // False when a record could not be appended (the list reports the cause).
    [[nodiscard]] bool Draw(RasterDrawList& list) const;

   private:
    Mode7PlaneConfig config_{};
    Row rows_[kMaxRows]{};
    uint32_t row_count_{};
    float centre_x_{};
    bool valid_{};
};

}  // namespace micropixel

#endif
