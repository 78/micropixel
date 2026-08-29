#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::square_720 {

struct Layout final {
    static constexpr int32_t kWidth = 720;
    static constexpr int32_t kHeight = 720;
    static constexpr int32_t kHallLeft = 40;
    static constexpr int32_t kHallTop = 208;
    static constexpr int32_t kHallViewportWidth = 680;
    static constexpr int32_t kHallCardWidth = 202;
    static constexpr int32_t kHallCardHeight = 254;
    static constexpr int32_t kHallCardGap = 17;
    static constexpr int32_t kHallScrollTrackWidth = 140;
    static constexpr int32_t kHallScrollTrackHeight = 6;
    static constexpr int32_t kStatusDialogVisibleY = 36;
    static constexpr int32_t kStatusDialogHiddenY = -508;
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::square_720
