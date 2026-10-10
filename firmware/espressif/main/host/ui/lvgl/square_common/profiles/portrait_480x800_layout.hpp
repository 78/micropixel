#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::portrait_480x800 {

struct Layout final {
    static constexpr int32_t kWidth = 480;
    static constexpr int32_t kHeight = 800;
    // Cover Flow: the centred card spans half the screen and the track step is
    // 158/256 of a card, so neighbours overlap it (negative gap).
    static constexpr int32_t kHallLeft = 0;
    static constexpr int32_t kHallTop = 240;
    static constexpr int32_t kHallViewportWidth = 480;
    static constexpr int32_t kHallCardWidth = 240;
    static constexpr int32_t kHallCardHeight = 300;
    static constexpr int32_t kHallCardGap = -92;
    static constexpr int32_t kHallScrollTrackWidth = 120;
    static constexpr int32_t kHallScrollTrackHeight = 4;
    static constexpr int32_t kStatusDialogVisibleY = 16;
};

static_assert(Layout::kHallViewportWidth == Layout::kWidth - Layout::kHallLeft);

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::portrait_480x800
