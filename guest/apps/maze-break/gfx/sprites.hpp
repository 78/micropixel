#ifndef MICROPIXEL_APPS_MAZE_BREAK_GFX_SPRITES_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_GFX_SPRITES_HPP

#include <stdint.h>

namespace maze_break::gfx {

// Palette-indexed sprite with index 0 as the transparency key.
struct Sprite {
    int width{};
    int height{};
    const uint8_t* pixels{};

    [[nodiscard]] uint8_t At(int x, int y) const { return pixels[y * width + x]; }
};

enum SpriteId : uint8_t {
    kSprImpWalkA = 0,
    kSprImpWalkB,
    kSprImpAttack,
    kSprImpPain,
    kSprImpDead,
    kSprFireballA,
    kSprFireballB,
    kSprMedkit,
    kSprAmmo,
    kSprTorchA,
    kSprTorchB,
    kSprBarrel,
    kSprShotgun,
    kSprMuzzleFlash,
    kSprCount,
};

// Decodes the ASCII art into palette indices. Must run after BuildPalette().
void BuildSprites();
[[nodiscard]] const Sprite& SpriteFor(SpriteId id);

}  // namespace maze_break::gfx

#endif
