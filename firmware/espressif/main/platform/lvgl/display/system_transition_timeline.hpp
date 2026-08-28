#pragma once

#include <array>
#include <cstdint>

namespace micropixel::platform::lvgl {

// P4 and S31 expose the same PPA SRM scale precision. Keep the transition
// geometry and timing in integer PPA units so every board renders the same
// path without accumulating float-rounding or partially written edges.
struct SystemTransitionTimeline final {
    static constexpr uint32_t kScaleDenominator = 16U;
    static constexpr uint32_t kCardScaleUnits = 9U;
    static constexpr uint32_t kFullscreenScaleUnits = 32U;
    static constexpr uint32_t kInitialToHallScaleUnits = 30U;
    static constexpr uint32_t kFirstVisibleScaleUnits = 11U;
    static constexpr uint32_t kLastVisibleScaleUnits = 30U;
    static constexpr uint32_t kToHallFirstScaleUnits = 25U;
    static constexpr uint32_t kAnimatedFrameCount = 5U;
    static constexpr uint32_t kFinalFramebufferCount = 2U;
    static constexpr std::array<uint16_t, 6U> kStatusEnterProgress{{400U, 650U, 820U, 930U, 1000U, 1000U}};
    static constexpr std::array<uint16_t, 6U> kStatusExitProgress{{950U, 800U, 550U, 250U, 0U, 0U}};

    [[nodiscard]] static constexpr const std::array<uint16_t, 6U>& StatusProgress(bool entering) {
        return entering ? kStatusEnterProgress : kStatusExitProgress;
    }

    [[nodiscard]] static constexpr uint32_t AnimatedScaleUnits(bool to_hall, uint32_t frame) {
        if (kAnimatedFrameCount <= 1U) {
            return to_hall ? kFirstVisibleScaleUnits : kLastVisibleScaleUnits;
        }
        return to_hall ? kToHallFirstScaleUnits -
                             (kToHallFirstScaleUnits - kFirstVisibleScaleUnits) * frame / (kAnimatedFrameCount - 1U)
                       : kFirstVisibleScaleUnits +
                             (kLastVisibleScaleUnits - kFirstVisibleScaleUnits) * frame / (kAnimatedFrameCount - 1U);
    }

    [[nodiscard]] static constexpr uint32_t ScaledDimension(uint32_t intermediate_width, uint32_t scale_units) {
        return intermediate_width * scale_units / kScaleDenominator;
    }
};

}  // namespace micropixel::platform::lvgl
