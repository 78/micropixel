#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_320 {

struct Layout final {
    static constexpr int32_t kWidth = 320;
    static constexpr int32_t kHeight = 240;
    static constexpr int32_t kHallLeft = 12;
    static constexpr int32_t kHallTop = 88;
    static constexpr int32_t kHallViewportWidth = 308;
    static constexpr int32_t kHallCardWidth = 88;
    static constexpr int32_t kHallCardHeight = 120;
    static constexpr int32_t kHallCardGap = 8;
    static constexpr int32_t kHallScrollTrackWidth = 72;
    static constexpr int32_t kHallScrollTrackHeight = 3;
    static constexpr int32_t kStatusDialogVisibleY = 8;
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_320
