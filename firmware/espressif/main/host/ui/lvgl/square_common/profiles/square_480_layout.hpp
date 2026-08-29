#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::square_480 {

struct Layout final {
    static constexpr int32_t kWidth = 480;
    static constexpr int32_t kHeight = 480;
    static constexpr int32_t kHallLeft = 24;
    static constexpr int32_t kHallTop = 178;
    static constexpr int32_t kHallViewportWidth = 456;
    static constexpr int32_t kHallCardWidth = 135;
    static constexpr int32_t kHallCardHeight = 174;
    static constexpr int32_t kHallCardGap = 9;
    static constexpr int32_t kHallScrollTrackWidth = 96;
    static constexpr int32_t kHallScrollTrackHeight = 4;
    static constexpr int32_t kStatusDialogVisibleY = 16;
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::square_480
