// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::portrait_800x1280 {

// The Hall keeps a 40 px side margin and a 720 px viewport. Three 226 px
// cards plus two 21 px gaps fill the row exactly; the grid uses three rows.
struct Layout final {
    static constexpr int32_t kWidth = 800;
    static constexpr int32_t kHeight = 1280;
    static constexpr int32_t kHallLeft = 40;
    static constexpr int32_t kHallTop = 208;
    static constexpr int32_t kHallViewportWidth = 720;
    static constexpr int32_t kHallCardWidth = 226;
    static constexpr int32_t kHallCardHeight = 244;
    static constexpr int32_t kHallCardCoverSize = 200;
    static constexpr int32_t kHallCardGap = 21;
    static constexpr int32_t kHallScrollTrackWidth = 140;
    static constexpr int32_t kHallScrollTrackHeight = 6;
    static constexpr int32_t kStatusDialogVisibleY = 24;
    static constexpr int32_t kStatusDialogHiddenY = -600;
};

static_assert(3 * Layout::kHallCardWidth + 2 * Layout::kHallCardGap == Layout::kHallViewportWidth);
static_assert(Layout::kHallLeft + Layout::kHallViewportWidth + Layout::kHallLeft == Layout::kWidth);

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::portrait_800x1280