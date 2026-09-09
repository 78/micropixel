#include "apps/tomb-explorer/gfx/palette.hpp"

namespace tomb::gfx {
namespace {

struct Rgb final {
    uint8_t r, g, b;
};

// Brightest colour of each ramp; steps darken towards black.
constexpr Rgb kRampBright[16] = {
    {200U, 200U, 205U},  // kGray
    {225U, 200U, 150U},  // kSand
    {205U, 150U, 80U},   // kOchre
    {140U, 95U, 60U},    // kBrown
    {95U, 140U, 70U},    // kMoss
    {60U, 150U, 140U},   // kTeal
    {70U, 95U, 190U},    // kBlue
    {240U, 200U, 70U},   // kGold
    {180U, 55U, 45U},    // kRed
    {230U, 175U, 135U},  // kSkin
    {70U, 45U, 30U},     // kHair
    {50U, 110U, 150U},   // kCloth
    {110U, 70U, 40U},    // kLeather
    {235U, 225U, 200U},  // kBone
    {40U, 90U, 130U},    // kWater
    {255U, 255U, 255U},  // kWhite
};

constexpr uint16_t Rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return static_cast<uint16_t>(((r & 0xF8U) << 8U) | ((g & 0xFCU) << 3U) | (b >> 3U));
}

}  // namespace

void BuildLitPalette(std::span<uint16_t> entries) {
    if (entries.size() < kLightLevels * 256U) return;
    for (uint32_t light = 0U; light < kLightLevels; ++light) {
        // Darkness leans blue, as torch-lit stone does, and never quite reaches black
        // so distant geometry keeps its shape.
        const uint32_t light_scale = 24U + light * (256U - 24U) / (kLightLevels - 1U);  // 24..256
        uint16_t* row = entries.data() + light * 256U;
        for (uint32_t ramp = 0U; ramp < 16U; ++ramp) {
            const Rgb bright = kRampBright[ramp];
            for (uint32_t step = 0U; step < kRampSteps; ++step) {
                const uint32_t step_scale = 40U + step * (256U - 40U) / (kRampSteps - 1U);  // 40..256
                const uint32_t scale = step_scale * light_scale / 256U;
                uint32_t r = bright.r * scale / 256U;
                uint32_t g = bright.g * scale / 256U;
                uint32_t b = bright.b * scale / 256U;
                if (light < 6U) {
                    b = b + (6U - light) * 3U;
                    if (b > 255U) b = 255U;
                }
                row[ramp * kRampSteps + step] = Rgb565(r, g, b);
            }
        }
        row[0] = Rgb565(0U, 0U, 0U);
    }
}

}  // namespace tomb::gfx
