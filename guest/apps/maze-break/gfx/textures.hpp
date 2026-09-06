#ifndef MICROPIXEL_APPS_MAZE_BREAK_GFX_TEXTURES_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GFX_TEXTURES_HPP

#include <stdint.h>

#include "apps/maze-break/gfx/view_config.hpp"

namespace maze_break::gfx {

enum TextureId : uint8_t {
    kTexBrick = 0,
    kTexStone,
    kTexTech,
    kTexFlesh,
    kTexWood,
    kTexDoor,
    kTexExit,
    kTexFloor,
    kTexCeiling,
    kTexCount,
};

// Palette-indexed kTextureSize^2 (128x128) textures generated procedurally at
// start, on a 64x64 design grid stretched by textures.cpp, so the App
// ships no binary assets. Wall textures (everything except floor/ceiling) are
// stored column-major, `t[(x << kTextureShift) | y]`, the layout the Host
// COLUMN kernel samples; floor and ceiling are row-major for the SPAN_PAIR
// kernel. They are uploaded to the Host once and never read here afterwards.
using Texture = uint8_t[kTextureSize * kTextureSize];

[[nodiscard]] constexpr bool ColumnMajor(TextureId id) { return id != kTexFloor && id != kTexCeiling; }

void BuildTextures();
[[nodiscard]] const Texture& TextureFor(TextureId id);

}  // namespace maze_break::gfx

#endif
