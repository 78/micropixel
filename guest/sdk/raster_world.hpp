#ifndef MICROPIXEL_SDK_RASTER_WORLD_HPP
#define MICROPIXEL_SDK_RASTER_WORLD_HPP

#include <stdint.h>

namespace micropixel {

// Vocabulary shared by the SDK's 2.5D front ends (Raycaster today; planar
// Mode-7 grounds and sphere views follow the same pattern). Each front end
// turns App geometry into RasterDrawList records; the Host INDEX8 + lit
// palette kernels do the per-pixel work. Nothing here touches the ABI.

// Distance lighting: level = curve_levels * full_distance / (full_distance +
// distance * falloff), clamped to [minimum, levels - 1]. `side_shade` is the
// extra darkening a front end applies to secondary faces (a raycaster's
// y-facing walls). The lit palette uploaded through RasterResources must have at
// least `levels` rows.
struct DistanceLighting final {
    uint8_t levels{16};
    uint8_t minimum{2};
    uint8_t side_shade{4};
    float curve_levels{31.0F};
    float full_distance{2.6F};
    float falloff{1.05F};
};

// Lookup table built from DistanceLighting, indexed by distance * 16 and
// clamped at 32 world units. Fixed size so front ends embed it by value.
class LightTable final {
   public:
    static constexpr int kEntries = 512;

    // False when levels is zero, minimum is not below levels, or the curve
    // parameters are not positive; the table is left unchanged.
    [[nodiscard]] bool Build(const DistanceLighting& lighting);
    [[nodiscard]] uint8_t LightFor(float distance) const;
    [[nodiscard]] uint8_t brightest() const { return brightest_; }
    [[nodiscard]] uint8_t levels() const { return levels_; }
    [[nodiscard]] uint8_t side_shade() const { return side_shade_; }

   private:
    uint8_t entries_[kEntries]{};
    uint8_t brightest_{};
    uint8_t levels_{};
    uint8_t side_shade_{};
};

// One camera-facing sprite standing on the ground of a 2.5D world. `height`
// and `lift` are fractions of the world's unit height (a raycaster's wall
// height). The texture is kColumnMajor with index 0 transparent,
// `texture_width` x `texture_height` texels at the origin.
struct Billboard final {
    float x{};
    float y{};
    float height{1.0F};
    float lift{};
    uint8_t texture_slot{};
    uint16_t texture_width{};
    uint16_t texture_height{};
    // Ignore distance lighting and draw at the brightest level.
    bool self_lit{};
};

}  // namespace micropixel

#endif
