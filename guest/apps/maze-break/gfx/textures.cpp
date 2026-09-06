#include "apps/maze-break/gfx/textures.hpp"

#include "apps/maze-break/gfx/palette.hpp"
#include "apps/maze-break/rc_math.hpp"

namespace maze_break::gfx {
namespace {

Texture gTextures[kTexCount];

// Deterministic integer hash so every boot generates identical textures.
uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393U + y * 668265263U + seed * 2246822519U;
    h = (h ^ (h >> 13)) * 1274126177U;
    return h ^ (h >> 16);
}

int Noise(int x, int y, uint32_t seed, int range) {
    return static_cast<int>(Hash(x, y, seed) % (range * 2 + 1)) - range;
}

void Fill(Texture& t, uint8_t index) {
    for (auto& p : t) {
        p = index;
    }
}

void Put(Texture& t, int x, int y, uint8_t index) {
    t[((y & kTextureMask) << kTextureShift) | (x & kTextureMask)] = index;
}

void Rect(Texture& t, int x0, int y0, int w, int h, uint8_t index) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            Put(t, x, y, index);
        }
    }
}

// The builders describe every feature on a 64x64 design grid; S stretches
// that grid to kTextureSize so a larger texture keeps the same bricks, panels
// and signs with finer per-texel grain rather than more of them.
constexpr int S = kTextureSize / 64;
static_assert(S >= 1 && kTextureSize % 64 == 0);

// Rect in design units.
void RectD(Texture& t, int x0, int y0, int w, int h, uint8_t index) { Rect(t, x0 * S, y0 * S, w * S, h * S, index); }

void BuildBrick(Texture& t) {
    constexpr int kBrickW = 16 * S;
    constexpr int kBrickH = 8 * S;
    for (int y = 0; y < kTextureSize; ++y) {
        const int row = y / kBrickH;
        const int offset = (row & 1) ? kBrickW / 2 : 0;
        for (int x = 0; x < kTextureSize; ++x) {
            const int bx = (x + offset) / kBrickW;
            const bool mortar = (y % kBrickH) < S || ((x + offset) % kBrickW) < S;
            if (mortar) {
                Put(t, x, y, Index(kWarmGray, 3 + Noise(x, y, 7, 1)));
                continue;
            }
            const int base = 7 + Noise(bx, row, 11, 2);
            const int grain = Noise(x, y, 13, 1);
            const Ramp ramp = (Hash(bx, row, 17) % 5 == 0) ? kRed : kBrown;
            Put(t, x, y, Index(ramp, base + grain));
        }
    }
}

void BuildStone(Texture& t) {
    // Irregular block widths per 16-row course.
    constexpr int kCourseH = 16 * S;
    for (int course = 0; course < 4; ++course) {
        int x = -static_cast<int>(Hash(course, 0, 23) % 10) * S;
        int block = 0;
        while (x < kTextureSize) {
            const int w = (12 + static_cast<int>(Hash(course, block, 29) % 12)) * S;
            const int level = 6 + Noise(course, block, 31, 2);
            for (int yy = 0; yy < kCourseH; ++yy) {
                for (int xx = 0; xx < w; ++xx) {
                    const int px = x + xx;
                    const int py = course * kCourseH + yy;
                    if (px < 0 || px >= kTextureSize) {
                        continue;
                    }
                    const bool edge = yy < S || yy >= kCourseH - S || xx < S || xx >= w - S;
                    const int shade = edge ? 3 : level + Noise(px, py, 37, 1) - (yy > 12 * S ? 1 : 0);
                    Put(t, px, py, Index(kWarmGray, shade));
                }
            }
            x += w;
            ++block;
        }
    }
}

void BuildTech(Texture& t) {
    Fill(t, Index(kSteel, 6));
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const int panel_y = (y % (32 * S)) / S;
            const int dx = x / S;
            const bool seam = panel_y == 0 || panel_y == 31 || dx == 0 || dx == 31 || dx == 32 || dx == 63;
            if (seam) {
                Put(t, x, y, Index(kSteel, 3));
            } else if (Noise(x, y, 41, 8) > 6) {
                Put(t, x, y, Index(kSteel, 7));
            }
        }
    }
    // Rivets in the panel corners.
    for (int py = 0; py < 2; ++py) {
        for (int px = 0; px < 2; ++px) {
            for (int ry = 0; ry < 2; ++ry) {
                for (int rx = 0; rx < 2; ++rx) {
                    RectD(t, px * 32 + 3 + rx * 24, py * 32 + 3 + ry * 24, 2, 2, Index(kSteel, 11));
                }
            }
        }
    }
    // Horizontal light strip with a blinking-pattern look.
    for (int x = 4; x < 60; ++x) {
        const int pulse = ((x / 6) % 2 == 0) ? 13 : 9;
        RectD(t, x, 29, 1, 6, Index(kCyan, pulse));
    }
    RectD(t, 3, 28, 58, 1, Index(kSteel, 2));
    RectD(t, 3, 35, 58, 1, Index(kSteel, 2));
}

void BuildFlesh(Texture& t) {
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const float fx = x * (0.11F / S);
            const float fy = y * (0.13F / S);
            const float blob = math::Sin(fx * 1.7F + math::Cos(fy * 2.1F)) * math::Cos(fy * 1.3F - fx * 0.7F);
            int level = 4 + static_cast<int>(blob * 2.5F) + Noise(x, y, 43, 1);
            Ramp ramp = kRed;
            const float vein = math::Sin(fy * 3.0F + math::Sin(fx * 2.2F) * 2.0F);
            if (vein > 0.92F) {
                ramp = kPurple;
                level = 6;
            }
            Put(t, x, y, Index(ramp, level));
        }
    }
}

void BuildWood(Texture& t) {
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const int plank = x / (16 * S);
            const int lx = (x % (16 * S)) / S;
            if (lx == 0 || lx == 15) {
                Put(t, x, y, Index(kBrown, 2));
                continue;
            }
            const float grain = math::Sin((y / S + plank * 13) * 0.35F + lx * 0.6F);
            const int level = 6 + static_cast<int>(grain * 1.8F) + Noise(x, y, 47, 1) + (plank % 2);
            Put(t, x, y, Index(kBrown, level));
        }
    }
    // Nail heads on each plank.
    for (int plank = 0; plank < 4; ++plank) {
        RectD(t, plank * 16 + 7, 6, 2, 2, Index(kGray, 9));
        RectD(t, plank * 16 + 7, 56, 2, 2, Index(kGray, 9));
    }
}

void BuildDoor(Texture& t) {
    Fill(t, Index(kSteel, 5));
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            if (Noise(x, y, 53, 6) > 4) {
                Put(t, x, y, Index(kSteel, 6));
            }
        }
    }
    // Frame and raised inner panel.
    RectD(t, 0, 0, 64, 2, Index(kSteel, 2));
    RectD(t, 0, 62, 64, 2, Index(kSteel, 2));
    RectD(t, 0, 0, 2, 64, Index(kSteel, 2));
    RectD(t, 62, 0, 2, 64, Index(kSteel, 2));
    RectD(t, 6, 6, 52, 1, Index(kSteel, 9));
    RectD(t, 6, 6, 1, 40, Index(kSteel, 9));
    RectD(t, 6, 45, 52, 1, Index(kSteel, 3));
    RectD(t, 57, 6, 1, 40, Index(kSteel, 3));
    // Centre split line.
    RectD(t, 31, 2, 2, 60, Index(kSteel, 2));
    // Small viewing window.
    RectD(t, 22, 12, 20, 10, Index(kGray, 2));
    RectD(t, 23, 13, 18, 8, Index(kCyan, 7));
    RectD(t, 25, 14, 6, 2, Index(kCyan, 12));
    // Hazard stripes along the bottom.
    for (int y = 50 * S; y < 60 * S; ++y) {
        for (int x = 4 * S; x < 60 * S; ++x) {
            const bool yellow = ((x + y) / (5 * S)) % 2 == 0;
            Put(t, x, y, yellow ? Index(kYellow, 12) : Index(kGray, 2));
        }
    }
}

void BuildExit(Texture& t) {
    Fill(t, Index(kMoss, 4));
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            if (Noise(x, y, 59, 6) > 4) {
                Put(t, x, y, Index(kMoss, 5));
            }
        }
    }
    RectD(t, 0, 0, 64, 3, Index(kGray, 4));
    RectD(t, 0, 61, 64, 3, Index(kGray, 4));
    RectD(t, 0, 0, 3, 64, Index(kGray, 4));
    RectD(t, 61, 0, 3, 64, Index(kGray, 4));
    // Lit sign: bright green square with an upward arrow.
    RectD(t, 16, 12, 32, 32, Index(kGreen, 4));
    RectD(t, 18, 14, 28, 28, Index(kGreen, 13));
    for (int i = 0; i < 10 * S; ++i) {
        Rect(t, 32 * S - i, 18 * S + i, 1 + i * 2, 1, Index(kGreen, 5));
    }
    RectD(t, 29, 28, 7, 10, Index(kGreen, 5));
    // "Power" strip under the sign.
    for (int x = 12; x < 52; x += 4) {
        RectD(t, x, 50, 2, 3, Index(kGreen, 11));
    }
}

void BuildFloor(Texture& t) {
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const bool grout = (x % (32 * S)) < S || (y % (32 * S)) < S;
            if (grout) {
                Put(t, x, y, Index(kGray, 2));
                continue;
            }
            const int tile = (x / (32 * S)) + (y / (32 * S)) * 2;
            const int level = 5 + (tile % 2) + Noise(x, y, 61, 1);
            const bool grime = Noise(x / (3 * S), y / (3 * S), 67, 10) > 7;
            Put(t, x, y, Index(kGray, grime ? level - 2 : level));
        }
    }
}

void BuildCeiling(Texture& t) {
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const bool seam = (x % (32 * S)) < S || (y % (32 * S)) < S;
            const int level = seam ? 1 : 3 + Noise(x, y, 71, 1);
            Put(t, x, y, Index(kWarmGray, level));
        }
    }
    // One recessed light per texture tile.
    RectD(t, 24, 28, 16, 8, Index(kGray, 6));
    RectD(t, 26, 30, 12, 4, Index(kWhite, 14));
}

}  // namespace

void BuildTextures() {
    BuildBrick(gTextures[kTexBrick]);
    BuildStone(gTextures[kTexStone]);
    BuildTech(gTextures[kTexTech]);
    BuildFlesh(gTextures[kTexFlesh]);
    BuildWood(gTextures[kTexWood]);
    BuildDoor(gTextures[kTexDoor]);
    BuildExit(gTextures[kTexExit]);
    BuildFloor(gTextures[kTexFloor]);
    BuildCeiling(gTextures[kTexCeiling]);
    // Builders draw row-major; rotate wall textures into column-major storage.
    for (int id = 0; id < kTexCount; ++id) {
        if (!ColumnMajor(static_cast<TextureId>(id))) {
            continue;
        }
        Texture& t = gTextures[id];
        for (int y = 0; y < kTextureSize; ++y) {
            for (int x = y + 1; x < kTextureSize; ++x) {
                const uint8_t a = t[(y << kTextureShift) | x];
                t[(y << kTextureShift) | x] = t[(x << kTextureShift) | y];
                t[(x << kTextureShift) | y] = a;
            }
        }
    }
}

const Texture& TextureFor(TextureId id) { return gTextures[id < kTexCount ? id : kTexBrick]; }

}  // namespace maze_break::gfx
