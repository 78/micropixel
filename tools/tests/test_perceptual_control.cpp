#include <cstdint>
#include <cstdio>

#include "platform/metalio-claw4/perceptual_control.hpp"

namespace {

using micropixel::platform::metalio_claw4::kBrightnessOutputFloor;
using micropixel::platform::metalio_claw4::kPerceptualControlScale;
using micropixel::platform::metalio_claw4::kVolumeOutputFloor;
using micropixel::platform::metalio_claw4::PerceptualBrightnessOutputPerTenThousand;
using micropixel::platform::metalio_claw4::PerceptualVolumeOutputPerTenThousand;
using micropixel::platform::metalio_claw4::ScaleAudioOutputSample;

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
           Check(PerceptualVolumeOutputPerTenThousand(10U) == 447U, "ten percent must follow the dB curve") &&
           Check(PerceptualVolumeOutputPerTenThousand(15U) == 531U, "fifteen percent must follow the dB curve") &&
           Check(PerceptualVolumeOutputPerTenThousand(50U) == 1778U,
                 "fifty percent volume must be fifteen dB below full scale") &&
           Check(PerceptualVolumeOutputPerTenThousand(90U) == 7079U,
                 "ninety percent volume must be three dB below full scale") &&
           Check(PerceptualVolumeOutputPerTenThousand(100U) == kPerceptualControlScale,
                 "one hundred percent must remain full output") &&
           Check(PerceptualVolumeOutputPerTenThousand(255U) == kPerceptualControlScale,
                 "out-of-range input must clamp to full output");
}

bool BrightnessZeroUsesTheSafePanelFloor() {
    return Check(PerceptualBrightnessOutputPerTenThousand(0U) == kBrightnessOutputFloor,
                 "zero percent brightness must retain a visible panel output") &&
           Check(PerceptualBrightnessOutputPerTenThousand(1U) == kBrightnessOutputFloor,
                 "one percent brightness must remain at the rounded safe panel floor") &&
           Check(PerceptualBrightnessOutputPerTenThousand(10U) == 361U,
                 "ten percent brightness must follow the gamma curve") &&
           Check(PerceptualBrightnessOutputPerTenThousand(50U) == 2411U,
                 "fifty percent brightness must reserve range for the upper half") &&
           Check(PerceptualBrightnessOutputPerTenThousand(90U) == 7993U,
                 "ninety percent brightness must follow the gamma curve") &&
           Check(PerceptualBrightnessOutputPerTenThousand(100U) == kPerceptualControlScale,
                 "one hundred percent brightness must remain full output");
}

bool LaterSliderStepsHaveMoreOutputRange() {
    const uint32_t low_step = PerceptualVolumeOutputPerTenThousand(20U) - PerceptualVolumeOutputPerTenThousand(10U);
    const uint32_t high_step = PerceptualVolumeOutputPerTenThousand(90U) - PerceptualVolumeOutputPerTenThousand(80U);
    return Check(high_step > low_step, "upper slider steps must change output more than lower slider steps");
}

bool AudioOutputIsTransparentAndSaturatesSafely() {
    return Check(ScaleAudioOutputSample(4000, 10000U) == 4000, "full volume must preserve the Guest sample") &&
           Check(ScaleAudioOutputSample(4000, 5000U) == 2000, "Host volume must only attenuate the Guest sample") &&
           Check(ScaleAudioOutputSample(4000, 0U) == 0, "muted volume must remain silent") &&
           Check(ScaleAudioOutputSample(40000, 10000U) == 32767, "positive overflow must saturate safely") &&
           Check(ScaleAudioOutputSample(-40000, 10000U) == -32768, "negative overflow must saturate safely") &&
           Check(ScaleAudioOutputSample(4000, 65535U) == 4000, "out-of-range master volume must clamp safely");
}

}  // namespace

int main() {
    return VolumeCurveHasExpectedAnchors() && BrightnessZeroUsesTheSafePanelFloor() &&
                   LaterSliderStepsHaveMoreOutputRange() && AudioOutputIsTransparentAndSaturatesSafely()
               ? 0
               : 1;
}
