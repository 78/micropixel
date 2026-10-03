#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_800x480 {

struct Layout final {
    static constexpr int32_t kWidth = 800;
    static constexpr int32_t kHeight = 480;
    // Cover Flow: the track step is 158/256 of a card, so neighbours overlap
    // the centred card (negative gap).
    static constexpr int32_t kHallLeft = 0;
    static constexpr int32_t kHallTop = 130;
    static constexpr int32_t kHallViewportWidth = 800;
    static constexpr int32_t kHallCardWidth = 200;
    static constexpr int32_t kHallCardHeight = 250;
    static constexpr int32_t kHallCardGap = -77;
    static constexpr int32_t kHallScrollTrackWidth = 140;
    static constexpr int32_t kHallScrollTrackHeight = 5;
    static constexpr int32_t kStatusDialogVisibleY = 16;
};

static_assert(Layout::kHallViewportWidth == Layout::kWidth - Layout::kHallLeft);

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_800x480
