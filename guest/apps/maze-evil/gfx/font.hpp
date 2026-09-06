#ifndef MICROPIXEL_APPS_MAZE_BREAK_GFX_FONT_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GFX_FONT_HPP

#include <stdint.h>

namespace maze_break::gfx {

inline constexpr int kGlyphWidth = 5;
inline constexpr int kGlyphHeight = 7;

// The fixed 5x7 glyph set (digits, upper-case letters, a few symbols) packed
// into one INDEX8 atlas for the Host raster kernels: 8x8 cells, 16 per row,
// texel 1 where the glyph is set and 0 (transparent) elsewhere. Text is drawn
// as one SOLID_COLOR sprite record per glyph. Lower-case letters map to
// upper-case.
inline constexpr int kGlyphCell = 8;
inline constexpr int kGlyphAtlasWidth = 128;
inline constexpr int kGlyphAtlasHeight = 32;
inline constexpr int kGlyphAtlasBytes = kGlyphAtlasWidth * kGlyphAtlasHeight;

// Writes the atlas column-major (texel(u, v) = atlas[u * kGlyphAtlasHeight + v]).
void BuildGlyphAtlas(uint8_t* atlas);
// Top-left atlas texel of `symbol`; false for a symbol without a glyph.
[[nodiscard]] bool GlyphCell(char symbol, int& u0, int& v0);
[[nodiscard]] int TextWidth(const char* text, int scale);

}  // namespace maze_break::gfx

#endif
