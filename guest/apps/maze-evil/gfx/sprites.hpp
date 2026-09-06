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
    kSprTorchC,
    kSprTorchD,
    kSprCount,
};

inline constexpr SpriteId kTorchFrames[] = {kSprTorchA, kSprTorchB, kSprTorchC, kSprTorchD};

[[nodiscard]] constexpr bool IsTorchSprite(SpriteId id) {
    return id == kSprTorchA || id == kSprTorchB || id == kSprTorchC || id == kSprTorchD;
}

// Art is quantized offline and stored as immutable palette-indexed pixels.
[[nodiscard]] const Sprite& SpriteFor(SpriteId id);

}  // namespace maze_break::gfx

#endif
