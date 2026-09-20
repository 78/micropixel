#include <cstdint>
#include <cstdio>

#include "platform/audio/volume_curve.hpp"
#include "platform/controllers/brightness_curve.hpp"

namespace {

using micropixel::platform::audio::ApplyHostOutputGain;
using micropixel::platform::audio::kVolumeControlScale;
using micropixel::platform::audio::ScaleOutputSample;
using micropixel::platform::audio::VolumeOutputPerTenThousand;
using micropixel::platform::controllers::BrightnessOutputPerTenThousand;
using micropixel::platform::controllers::kBrightnessControlScale;
using micropixel::platform::controllers::kBrightnessOutputFloor;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

bool VolumeCurveHasExpectedAnchors() {
    return Check(VolumeOutputPerTenThousand(0U) == 0U, "zero percent volume must remain muted") &&
           Check(VolumeOutputPerTenThousand(1U) == 1U, "one percent must retain the smallest nonzero gain") &&
           Check(VolumeOutputPerTenThousand(10U) == 100U, "ten percent must give one percent amplitude") &&
           Check(VolumeOutputPerTenThousand(15U) == 225U, "fifteen percent must give 2.25 percent amplitude") &&
           Check(VolumeOutputPerTenThousand(50U) == 2500U, "fifty percent must give twenty-five percent amplitude") &&
           Check(VolumeOutputPerTenThousand(70U) == 4900U, "default volume must give forty-nine percent amplitude") &&
           Check(VolumeOutputPerTenThousand(80U) == 6400U, "eighty percent must give sixty-four percent amplitude") &&
           Check(VolumeOutputPerTenThousand(90U) == 8100U, "ninety percent must give eighty-one percent amplitude") &&
           Check(VolumeOutputPerTenThousand(100U) == kVolumeControlScale,
                 "one hundred percent must remain full output") &&
           Check(VolumeOutputPerTenThousand(255U) == kVolumeControlScale,
                 "out-of-range input must clamp to full output");
}

bool VolumeCurveIsMonotonicAndClamped() {
    uint16_t previous = VolumeOutputPerTenThousand(0U);
    for (uint32_t percent = 1U; percent <= 255U; ++percent) {
        const uint16_t output = VolumeOutputPerTenThousand(static_cast<uint8_t>(percent));
        if (!Check(percent <= 100U ? output > previous : output == kVolumeControlScale,
                   "volume must rise at every valid step and clamp above one hundred percent")) {
            return false;
        }
        previous = output;
    }
    return true;
}

bool BrightnessZeroUsesTheSafePanelFloor() {
    return Check(BrightnessOutputPerTenThousand(0U) == kBrightnessOutputFloor,
                 "zero percent brightness must retain a visible panel output") &&
           Check(BrightnessOutputPerTenThousand(1U) == kBrightnessOutputFloor,
                 "one percent brightness must remain at the rounded safe panel floor") &&
           Check(BrightnessOutputPerTenThousand(10U) == 361U, "ten percent brightness must follow the gamma curve") &&
           Check(BrightnessOutputPerTenThousand(50U) == 2411U,
                 "fifty percent brightness must reserve range for the upper half") &&
           Check(BrightnessOutputPerTenThousand(90U) == 7993U,
                 "ninety percent brightness must follow the gamma curve") &&
           Check(BrightnessOutputPerTenThousand(100U) == kBrightnessControlScale,
                 "one hundred percent brightness must remain full output") &&
           Check(micropixel::platform::controllers::VisibleBrightnessPercent(0U) == 3U,
                 "whole-percent panel drivers must retain the three-percent safety floor") &&
           Check(micropixel::platform::controllers::VisibleBrightnessPercent(100U) == 100U,
                 "whole-percent panel drivers must retain full output");
}

bool BrightnessCurveSupportsABoardSpecificFloor() {
    using micropixel::platform::controllers::BrightnessOutputPerTenThousand;
    using micropixel::platform::controllers::VisibleBrightnessPercent;
    constexpr uint32_t kS31BrightnessOutputFloor = BrightnessOutputPerTenThousand(30U);
    return Check(VisibleBrightnessPercent(0U, kS31BrightnessOutputFloor) == VisibleBrightnessPercent(30U),
                 "the S31 floor must match the original thirty-percent setting") &&
           Check(VisibleBrightnessPercent(0U, kS31BrightnessOutputFloor) == 10U,
                 "the S31 floor must account for gamma before reaching the panel") &&
           Check(VisibleBrightnessPercent(50U, kS31BrightnessOutputFloor) == 30U,
                 "a board-specific floor must retain the perceptual curve") &&
           Check(VisibleBrightnessPercent(100U, kS31BrightnessOutputFloor) == 100U,
                 "a board-specific floor must retain full output") &&
           Check(VisibleBrightnessPercent(255U, kS31BrightnessOutputFloor) == 100U,
                 "a board-specific floor must retain input clamping");
}

bool LaterSliderStepsHaveMoreOutputRange() {
    const uint32_t low_step = VolumeOutputPerTenThousand(20U) - VolumeOutputPerTenThousand(10U);
    const uint32_t high_step = VolumeOutputPerTenThousand(90U) - VolumeOutputPerTenThousand(80U);
    return Check(high_step > low_step, "upper slider steps must change output more than lower slider steps");
}

bool AudioOutputIsTransparentAndSaturatesSafely() {
    return Check(ScaleOutputSample(4000, 10000U) == 4000, "full volume must preserve the Guest sample") &&
           Check(ApplyHostOutputGain(10000, VolumeOutputPerTenThousand(80U), false) == 6400,
                 "eighty percent volume must apply sixty-four percent gain to the mixed output") &&
           Check(ApplyHostOutputGain(-10000, VolumeOutputPerTenThousand(50U), false) == -2500,
                 "fifty percent volume must apply twenty-five percent gain to negative samples") &&
           Check(ScaleOutputSample(4000, 5000U) == 2000, "Host volume must only attenuate the Guest sample") &&
           Check(ScaleOutputSample(4000, 0U) == 0, "muted volume must remain silent") &&
           Check(ScaleOutputSample(40000, 10000U) == 32767, "positive overflow must saturate safely") &&
           Check(ScaleOutputSample(-40000, 10000U) == -32768, "negative overflow must saturate safely") &&
           Check(ScaleOutputSample(4000, 65535U) == 4000, "out-of-range master volume must clamp safely");
}

bool HardwareMuteDoesNotOverwriteMasterVolume() {
    constexpr uint16_t saved_volume = 5000U;
    return Check(ApplyHostOutputGain(4000, saved_volume, true) == 0, "hardware mute must zero the Host output") &&
           Check(ApplyHostOutputGain(4000, saved_volume, false) == 2000,
                 "hardware unmute must restore the unchanged master-volume gain");
}

}  // namespace

int main() {
    return VolumeCurveHasExpectedAnchors() && VolumeCurveIsMonotonicAndClamped() &&
                   BrightnessZeroUsesTheSafePanelFloor() && BrightnessCurveSupportsABoardSpecificFloor() &&
                   LaterSliderStepsHaveMoreOutputRange() && AudioOutputIsTransparentAndSaturatesSafely() &&
                   HardwareMuteDoesNotOverwriteMasterVolume()
               ? 0
               : 1;
}
