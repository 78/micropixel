#include <cstdint>
#include <cstdio>

#include "platform/metalio-claw4/perceptual_control.hpp"

namespace {

using micropixel::platform::metalio_claw4::kBrightnessOutputFloor;
using micropixel::platform::metalio_claw4::kPerceptualControlScale;
using micropixel::platform::metalio_claw4::kVolumeOutputFloor;
using micropixel::platform::metalio_claw4::PerceptualBrightnessOutputPerTenThousand;
using micropixel::platform::metalio_claw4::PerceptualVolumeOutputPerTenThousand;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

bool VolumeCurveHasExpectedAnchors() {
    return Check(PerceptualVolumeOutputPerTenThousand(0U) == 0U, "zero percent volume must remain muted") &&
           Check(PerceptualVolumeOutputPerTenThousand(1U) > kVolumeOutputFloor,
                 "the first active setting must clear the hardware dead zone") &&
           Check(PerceptualVolumeOutputPerTenThousand(10U) == 834U, "ten percent must remain audibly active") &&
           Check(PerceptualVolumeOutputPerTenThousand(15U) == 1136U,
                 "fifteen percent must remain clear of the hardware dead zone") &&
           Check(PerceptualVolumeOutputPerTenThousand(50U) == 3938U,
                 "fifty percent volume must use the blended curve") &&
           Check(PerceptualVolumeOutputPerTenThousand(90U) == 8594U,
                 "ninety percent volume must preserve upper-range control") &&
           Check(PerceptualVolumeOutputPerTenThousand(100U) == kPerceptualControlScale,
                 "one hundred percent must remain full output") &&
           Check(PerceptualVolumeOutputPerTenThousand(255U) == kPerceptualControlScale,
                 "out-of-range input must clamp to full output");
}

bool BrightnessZeroUsesThePanelFloor() {
    return Check(PerceptualBrightnessOutputPerTenThousand(0U) == kBrightnessOutputFloor,
                 "zero percent brightness must map to fifteen percent panel output") &&
           Check(PerceptualBrightnessOutputPerTenThousand(10U) == 1968U,
                 "brightness must rise smoothly above its output floor") &&
           Check(PerceptualBrightnessOutputPerTenThousand(100U) == kPerceptualControlScale,
                 "one hundred percent brightness must remain full output");
}

bool LaterSliderStepsHaveMoreOutputRange() {
    const uint32_t low_step = PerceptualVolumeOutputPerTenThousand(20U) - PerceptualVolumeOutputPerTenThousand(10U);
    const uint32_t high_step = PerceptualVolumeOutputPerTenThousand(90U) - PerceptualVolumeOutputPerTenThousand(80U);
    return Check(high_step > low_step, "upper slider steps must change output more than lower slider steps");
}

}  // namespace

int main() {
    return VolumeCurveHasExpectedAnchors() && BrightnessZeroUsesThePanelFloor() && LaterSliderStepsHaveMoreOutputRange()
               ? 0
               : 1;
}
