#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::square_412 {

// SenseCAP Watcher. The panel is 412x412 and its cover is round, so the square
// frame is not the usable area: a point at vertical distance d from the centre
// of a diameter-412 circle has only 2*sqrt(206^2 - d^2) pixels of usable width,
// and the four corners of the frame are outside the glass entirely.
//
// Every token below therefore lives inside the largest axis-aligned square that
// fits in that circle: side 412/sqrt(2) = 291.3, so the inset is
// (412 - 291.3) / 2 = 60.3, rounded up to kRoundInset. Such a box touches the
// circle exactly at its corners, so anything inside it is visible at every
// angle. That is a provable rule the layout can be checked against, instead of
// judging each element separately.
//
// square_480 is the sibling round profile, but its tokens only inset by 24 px
// because that panel's active area is a rounded rectangle rather than a circle.
struct Layout final {
    static constexpr int32_t kWidth = 412;
    static constexpr int32_t kHeight = 412;
    static constexpr int32_t kRoundInset = 60;
    static constexpr int32_t kContentLeft = kRoundInset;
    static constexpr int32_t kContentRight = kWidth - kRoundInset;
    static constexpr int32_t kHallLeft = kContentLeft;
    static constexpr int32_t kHallTop = 165;
    static constexpr int32_t kHallViewportWidth = kWidth - kHallLeft;
    static constexpr int32_t kHallCardWidth = 108;
    static constexpr int32_t kHallCardHeight = 139;
    static constexpr int32_t kHallCardGap = 8;
    static constexpr int32_t kHallScrollTrackWidth = 82;
    static constexpr int32_t kHallScrollTrackHeight = 4;
    static constexpr int32_t kStatusDialogVisibleY = 44;
};

static_assert(Layout::kHallViewportWidth == Layout::kWidth - Layout::kHallLeft);
static_assert(Layout::kHallCardWidth * 3 + Layout::kHallCardGap * 2 <= Layout::kHallViewportWidth);
// The Hall band must sit inside the inscribed square, which is what keeps the
// outer cards clear of the cover.
static_assert(Layout::kHallTop >= Layout::kContentLeft);
static_assert(Layout::kHallTop + Layout::kHallCardHeight <= Layout::kContentRight);

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::square_412
