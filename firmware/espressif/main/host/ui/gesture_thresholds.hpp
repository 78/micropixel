#ifndef MICROPIXEL_HOST_UI_GESTURE_THRESHOLDS_HPP
#define MICROPIXEL_HOST_UI_GESTURE_THRESHOLDS_HPP

#include <cstdint>

namespace micropixel::host_ui::gesture_thresholds {

struct ExtentRatio final {
    int32_t numerator;
    int32_t denominator;
};

// Preserve the original 720x720 tuning while expressing every spatial
// threshold relative to the active display dimensions.
inline constexpr ExtentRatio kTopReservedEdge{1, 10};
inline constexpr ExtentRatio kBottomReservedEdge{7, 120};
inline constexpr ExtentRatio kSystemRecognitionDistance{7, 90};
inline constexpr ExtentRatio kSystemDirectionSlop{1, 18};
inline constexpr ExtentRatio kLayerGestureDistance{5, 72};
inline constexpr ExtentRatio kLayerGestureMinVelocity{1, 240};

constexpr int32_t ScaleExtent(int32_t extent, ExtentRatio ratio) {
    return (extent * ratio.numerator + ratio.denominator - 1) / ratio.denominator;
}

constexpr uint8_t ScaleExtentToUint8(int32_t extent, ExtentRatio ratio) {
    const int32_t scaled = ScaleExtent(extent, ratio);
    return scaled < 1 ? 1U : (scaled > UINT8_MAX ? UINT8_MAX : static_cast<uint8_t>(scaled));
}

constexpr int32_t GestureHintWidth(int32_t display_width) { return display_width / 5 > 80 ? display_width / 5 : 80; }

constexpr int32_t GestureHintHeight(int32_t display_width) { return display_width / 90 > 6 ? display_width / 90 : 6; }

constexpr int32_t GestureHintBottomMargin(int32_t display_width) {
    return display_width / 40 > 8 ? display_width / 40 : 8;
}

constexpr int32_t GestureHintBottomExtent(int32_t display_width) {
    return GestureHintHeight(display_width) + GestureHintBottomMargin(display_width);
}

}  // namespace micropixel::host_ui::gesture_thresholds

#endif
