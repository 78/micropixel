#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_GFX_PALETTE_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_GFX_PALETTE_HPP

#include <stdint.h>

#include <span>

namespace tomb::gfx {

// INDEX8 palette shared by every texture: 16 colour ramps x 16 brightness
// steps, index = ramp * 16 + step. Step 0 of ramp 0 (index 0) is the
// transparent key. tools/generate_textures.py mirrors this layout.
enum Ramp : uint8_t {
    kGray = 0,
    kSand,
    kOchre,
    kBrown,
    kMoss,
    kTeal,
    kBlue,
    kGold,
    kRed,
    kSkin,
    kHair,
    kCloth,
    kLeather,
    kBone,
    kWater,
    kWhite,
};

constexpr uint32_t kRampSteps = 16U;
// Light levels the Host lit palette holds; the MeshRenderer quantises to it.
constexpr uint32_t kLightLevels = 16U;

[[nodiscard]] constexpr uint8_t Index(Ramp ramp, uint32_t step) {
    if (step > kRampSteps - 1U) step = kRampSteps - 1U;
    return static_cast<uint8_t>(ramp * kRampSteps + step);
}

// Fills `entries` (kLightLevels x 256 canonical RGB565) for RasterResources::UploadLitPalette.
void BuildLitPalette(std::span<uint16_t> entries);

}  // namespace tomb::gfx

#endif
