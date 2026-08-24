#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_PERCEPTUAL_CONTROL_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_PERCEPTUAL_CONTROL_HPP

#include <cstdint>

namespace micropixel::platform::metalio_claw4 {

constexpr uint32_t kPerceptualControlScale = 10000U;
constexpr uint32_t kBrightnessOutputFloor = 1500U;
constexpr uint32_t kVolumeOutputFloor = 300U;

constexpr uint32_t BlendedControlOutputPerTenThousand(uint8_t percent) {
    const uint32_t clamped_percent = percent <= 100U ? percent : 100U;
    const uint32_t linear_output = clamped_percent * 100U;
    const uint32_t quadratic_output = clamped_percent * clamped_percent;
    return (linear_output + quadratic_output) / 2U;
}

constexpr uint32_t OutputAboveFloorPerTenThousand(uint8_t percent, uint32_t floor) {
    const uint32_t blended_output = BlendedControlOutputPerTenThousand(percent);
    return floor + ((kPerceptualControlScale - floor) * blended_output + kPerceptualControlScale / 2U) /
                       kPerceptualControlScale;
}

// Brightness 0% is the dimmest usable setting, not panel power-off. The
// physical backlight switch still bypasses this mapping when it needs true off.
constexpr uint32_t PerceptualBrightnessOutputPerTenThousand(uint8_t percent) {
    return OutputAboveFloorPerTenThousand(percent, kBrightnessOutputFloor);
}

// Volume 0% remains true mute. Active values start above the speaker dead zone.
constexpr uint32_t PerceptualVolumeOutputPerTenThousand(uint8_t percent) {
    return percent == 0U ? 0U : OutputAboveFloorPerTenThousand(percent, kVolumeOutputFloor);
}

}  // namespace micropixel::platform::metalio_claw4

#endif
