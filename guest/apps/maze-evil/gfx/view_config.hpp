#ifndef MICROPIXEL_APPS_MAZE_BREAK_GFX_VIEW_CONFIG_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GFX_VIEW_CONFIG_HPP

#include <stdint.h>

namespace maze_break::gfx {

// The render target is whatever HostSurface buffer the Host hands out, so
// the view size is a runtime value. These bounds size the per-column scratch
// arrays; any board up to 800 px wide works with one binary.
inline constexpr int kMaxViewWidth = 800;
inline constexpr int kMaxViewHeight = 800;

// Texture atlas geometry shared by walls, floor and ceiling.
inline constexpr int kTextureShift = 7;
inline constexpr int kTextureSize = 1 << kTextureShift;  // 128
inline constexpr int kTextureMask = kTextureSize - 1;

// Palette: 16 hue ramps of 16 brightness levels.
inline constexpr int kRampLevels = 16;
inline constexpr int kLightLevels = 16;

// Runtime description of the current target buffer. Pixels are drawn by the
// Host raster kernels, so the App never sees the buffer's byte order; every
// colour it hands over is canonical RGB565.
struct ViewConfig final {
    int width{};
    int height{};
    // HUD scale: 1 at 240 px wide, 2 at 480, 3 at 720.
    int hud_scale{1};
};

}  // namespace maze_break::gfx

#endif
