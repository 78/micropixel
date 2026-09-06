#ifndef MICROPIXEL_APPS_MAZE_BREAK_GFX_TEXTURES_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GFX_TEXTURES_HPP

#include <stdint.h>

#include "apps/maze-evil/gfx/view_config.hpp"

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

// Palette-indexed 128x128 source artwork, quantized offline by
// assets/source/convert_art.py. Wall textures are stored column-major,
// t[(x << kTextureShift) | y], for the Host COLUMN kernel; floor and ceiling
// are row-major for SPAN_PAIR. Immutable pixels are uploaded once at startup.
using Texture = uint8_t[kTextureSize * kTextureSize];

[[nodiscard]] constexpr bool ColumnMajor(TextureId id) { return id != kTexFloor && id != kTexCeiling; }

[[nodiscard]] const Texture& TextureFor(TextureId id);

}  // namespace maze_break::gfx

#endif
