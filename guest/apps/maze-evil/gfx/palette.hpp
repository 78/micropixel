#ifndef MICROPIXEL_APPS_MAZE_BREAK_GFX_PALETTE_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GFX_PALETTE_HPP

#include <stdint.h>

#include "apps/maze-evil/gfx/view_config.hpp"

namespace maze_break::gfx {

enum Ramp : uint8_t {
    kGray = 0,
    kWarmGray,
    kBrown,
    kRed,
    kOrange,
    kYellow,
    kGreen,
    kTeal,
    kBlue,
    kPurple,
    kSkin,
    kMoss,
    kSteel,
    kGold,
    kCyan,
    kWhite,
};

// Index 0 is reserved as the sprite transparency key; it is also the darkest
// gray, which no sprite art needs.
inline constexpr uint8_t kTransparent = 0;

[[nodiscard]] constexpr uint8_t Index(Ramp ramp, int level) {
    level = level < 1 ? 1 : (level > kRampLevels - 1 ? kRampLevels - 1 : level);
    return static_cast<uint8_t>(ramp * kRampLevels + level);
}

// One light level of the colormap: 256 palette entries as canonical RGB565.
using Colormap = uint16_t[256];

// Builds the 16 light-level colormaps; together they are the lit palette the
// Host raster kernels are given (kLightLevels x 256, contiguous from light 0).
void BuildPalette();
[[nodiscard]] const Colormap& ColormapFor(int light);
// Full-brightness colour of a palette index, for HUD text and fills.
[[nodiscard]] uint16_t PaletteRgb565(uint8_t index);

}  // namespace maze_break::gfx

#endif
