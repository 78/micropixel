#include "apps/maze-break/gfx/palette.hpp"

namespace maze_break::gfx {
namespace {

struct Rgb {
    uint8_t r, g, b;
};

// Full-brightness colour of each ramp; level 15 hits this, lower levels fade
// towards black with a slight gamma so mid-levels stay readable.
constexpr Rgb kRampBright[16] = {
    {205, 205, 205},  // gray
    {200, 182, 150},  // warm gray / stone
    {178, 112, 58},   // brown
    {235, 44, 32},    // red
    {245, 140, 40},   // orange
    {245, 222, 64},   // yellow
    {76, 205, 76},    // green
    {66, 186, 176},   // teal
    {80, 116, 235},   // blue
    {156, 76, 205},   // purple
    {238, 176, 146},  // skin
    {74, 118, 56},    // moss
    {132, 150, 178},  // steel
    {214, 176, 66},   // gold
    {96, 232, 242},   // cyan
    {255, 255, 255},  // white
};

// ((level + 1) / 16) ^ 1.25 for level 0..15; precomputed because the Guest
// links no libm and the table is only 16 entries.
constexpr float kLevelGamma[kRampLevels] = {
    0.031250F, 0.074325F, 0.123382F, 0.176777F, 0.233648F, 0.293453F, 0.355814F, 0.420448F,
    0.487139F, 0.555712F, 0.626024F, 0.697954F, 0.771399F, 0.846272F, 0.922495F, 1.000000F,
};

Rgb gPalette[256];
Colormap gColormap[kLightLevels];

// Green is quantised to 5 bits like red and blue. RGB888 panels expand RGB565
// by zero-filling the low bits (P4 PPA: r<<3, g<<2, b<<3), so a 6-bit green
// keeps one extra bit and every grey picks up as much as +4 green. With equal
// 5-bit quantisation the expanded channels match whether the scanout path
// zero-fills or replicates bits; the game's 256-colour palette never needs
// the 64 green levels.
[[nodiscard]] constexpr uint16_t Pack565(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xF8) << 3) | (b >> 3));
}

}  // namespace

void BuildPalette() {
    for (int ramp = 0; ramp < 16; ++ramp) {
        for (int level = 0; level < kRampLevels; ++level) {
            const float t = kLevelGamma[level];
            const Rgb bright = kRampBright[ramp];
            gPalette[ramp * kRampLevels + level] = {
                static_cast<uint8_t>(bright.r * t + 0.5F),
                static_cast<uint8_t>(bright.g * t + 0.5F),
                static_cast<uint8_t>(bright.b * t + 0.5F),
            };
        }
    }
    for (int light = 0; light < kLightLevels; ++light) {
        const float scale = (light + 1) / static_cast<float>(kLightLevels);
        for (int index = 0; index < 256; ++index) {
            const Rgb c = gPalette[index];
            gColormap[light][index] =
                Pack565(static_cast<int>(c.r * scale), static_cast<int>(c.g * scale), static_cast<int>(c.b * scale));
        }
    }
}

const Colormap& ColormapFor(int light) {
    if (light < 0) {
        light = 0;
    } else if (light >= kLightLevels) {
        light = kLightLevels - 1;
    }
    return gColormap[light];
}

uint16_t PaletteRgb565(uint8_t index) { return gColormap[kLightLevels - 1][index]; }

}  // namespace maze_break::gfx
