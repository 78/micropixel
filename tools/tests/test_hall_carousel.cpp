#include <cstdlib>
#include <iostream>

#include "platform/lvgl/ui/square_common/hall_carousel.hpp"
#include "platform/lvgl/ui/square_common/hall_cover_cache_policy.hpp"
#include "platform/lvgl/ui/square_common/hall_transition_policy.hpp"
#include "platform/lvgl/ui/square_common/profiles/square_480_layout.hpp"
#include "platform/lvgl/ui/square_common/profiles/square_720_layout.hpp"

namespace {

namespace lvgl = micropixel::platform::lvgl;
using HallCarousel = lvgl::square_common::HallCarouselPolicy<lvgl::square_common::profiles::square_720::Layout>;
using lvgl::square_common::HallCoverCachePolicy;
using lvgl::square_common::HallCoverCacheSlot;
using lvgl::square_common::HallLaunchBackgroundPlan;
using lvgl::square_common::HallVelocityTracker;
using lvgl::square_common::PlanHallLaunchBackground;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void CapacityAndGeometry() {
    Check(HallCarousel::MaxOffset(0U) == 0 && HallCarousel::MaxOffset(3U) == 0, "up to three Apps must not scroll");
    Check(HallCarousel::MaxScrollIndex(4U) == 1U && HallCarousel::MaxScrollIndex(7U) == 4U,
          "four to seven Apps must expose the expected snap positions");
    Check(HallCarousel::CardX(0U, 0) == 40 && HallCarousel::CardX(3U, 0) == 697,
          "the first view must show three cards and the leading edge of the fourth");
    Check(HallCarousel::CardX(6U, HallCarousel::MaxOffset(7U)) == 478,
          "the final snap must align the last three cards in the viewport");
}

void Square480Geometry() {
    using Square480Carousel =
        lvgl::square_common::HallCarouselPolicy<lvgl::square_common::profiles::square_480::Layout>;
    Check(Square480Carousel::CardX(0U, 0) == 24 && Square480Carousel::CardX(3U, 0) == 456,
          "the 480 profile must expose the leading edge of its fourth card");
    Check(Square480Carousel::kCardWidth == 135 && Square480Carousel::kCardHeight == 174,
          "the 480 profile must own independent card geometry");
    Check(Square480Carousel::MaxOffset(7U) == 576,
          "the shared carousel policy must derive scrolling from the 480 layout");
}

void DragClampingAndDirection() {
    Check(HallCarousel::DragOffset(7U, 0, -300) == 300, "left drag must move toward newer off-screen content");
    Check(HallCarousel::DragOffset(7U, 0, 120) == 0, "right drag at the first page must clamp");
    Check(HallCarousel::DragOffset(7U, HallCarousel::MaxOffset(7U), -120) == HallCarousel::MaxOffset(7U),
          "left drag at the final page must clamp");
    Check(HallCarousel::IsHorizontalDrag(-24, 8), "dominant horizontal motion must start carousel dragging");
    Check(!HallCarousel::IsHorizontalDrag(6, 2) && !HallCarousel::IsHorizontalDrag(20, 30),
          "small or vertically dominant motion must not start carousel dragging");
}

void VelocityAndFreeInertia() {
    HallVelocityTracker tracker;
    tracker.Reset(0, 0U);
    tracker.Add(10, 20000U);
    tracker.Add(20, 40000U);
    tracker.Add(30, 60000U);
    Check(tracker.Velocity(HallCarousel::kMaximumThrowVelocity) == 500,
          "trajectory fitting must recover constant touch velocity");

    tracker.Reset(0, 0U);
    tracker.Add(2, 10000U);
    tracker.Add(8, 20000U);
    tracker.Add(18, 30000U);
    tracker.Add(32, 40000U);
    const int32_t accelerating_velocity = tracker.Velocity(HallCarousel::kMaximumThrowVelocity);
    Check(accelerating_velocity >= 800 && accelerating_velocity <= 850,
          "weighted trajectory fitting must follow an accelerating gesture without overshoot");

    tracker.AddRelease(32, 65000U);
    const int32_t released_velocity = tracker.Velocity(HallCarousel::kMaximumThrowVelocity, 65000U);
    Check(released_velocity >= 600 && released_velocity <= 650,
          "a delayed duplicate release must decay rather than reverse the fitted velocity");

    const int32_t target = HallCarousel::InertiaTarget(7U, 123, 900);
    Check(target >= 560 && target <= 562, "exponential inertia must stop at a free pixel offset");
    Check(target % HallCarousel::kCardStep != 0, "inertia must not snap to a card step");
    Check(HallCarousel::InertiaTarget(7U, 800, 4000) == HallCarousel::MaxOffset(7U),
          "inertia must clamp at the content edge");
    const uint32_t duration = HallCarousel::InertiaDurationMs(123, target, 900);
    Check(duration >= 1800U && duration <= 1820U, "inertia must retain the exponential deceleration tail");
    Check(HallCarousel::InertiaOffset(123, target, 900, 0U) == 123,
          "exponential inertia must begin at the release position");
    Check(HallCarousel::InertiaOffset(123, target, 900, duration) == target,
          "exponential inertia must finish at its free-pixel target");
    Check(HallCarousel::InertiaDurationMs(100, 100, 900) == 0U, "a stationary target must not animate");
}

void ContinuousIndicator() {
    constexpr int32_t kTrackWidth = 140;
    const int32_t thumb_width = HallCarousel::ScrollThumbWidth(kTrackWidth, 7U);
    Check(thumb_width == 60, "the scroll thumb must represent three visible Apps out of seven");
    Check(HallCarousel::ScrollThumbX(kTrackWidth, thumb_width, 7U, 0) == 0,
          "the scroll thumb must begin at the left edge");
    Check(HallCarousel::ScrollThumbX(kTrackWidth, thumb_width, 7U, HallCarousel::MaxOffset(7U)) == 80,
          "the scroll thumb must reach the right edge continuously");
}

void CardRevealOffset() {
    Check(HallCarousel::RevealOffset(7U, 0, 3U) == 179,
          "a tapped card clipped by the right edge must be fully revealed");
    Check(HallCarousel::RevealOffset(20U, 10 * HallCarousel::kCardStep + 40, 10U) == 10 * HallCarousel::kCardStep,
          "a tapped card clipped by the left edge must be fully revealed");
    Check(HallCarousel::RevealOffset(7U, 123, 2U) == 123,
          "a fully visible tapped card must preserve the free-pixel offset");
    Check(HallCarousel::RevealOffset(7U, 123, 7U) == 123, "an invalid card index must not move the carousel");
}

void BoundedCoverWindow() {
    Check(HallCarousel::CoverWindowFirst(50U, 0) == 0U && HallCarousel::CoverWindowLast(50U, 0) == 5U,
          "the first viewport must load only visible covers plus one forward prefetch");
    Check(HallCarousel::CoverWindowFirst(50U, 10 * HallCarousel::kCardStep + 40) == 9U &&
              HallCarousel::CoverWindowLast(50U, 10 * HallCarousel::kCardStep + 40) == 15U,
          "a free-pixel offset must retain one cover on each side of four intersecting cards");
    Check(HallCarousel::CoverWindowLast(50U, HallCarousel::MaxOffset(50U)) == 50U,
          "the final cover window must clamp to the Catalog");
    Check(HallCarousel::kMaximumCachedCovers == 6U,
          "decoded Hall cover memory must remain bounded independently of App count");
}

void CoverCacheReplacementAfterAppUpdate() {
    constexpr std::array<HallCoverCacheSlot, HallCarousel::kMaximumCachedCovers> kFullCache{{
        {.occupied = true, .key = 10U, .app_index = 9U},
        {.occupied = true, .key = 11U, .app_index = 10U},
        {.occupied = true, .key = 12U, .app_index = 11U},
        {.occupied = true, .key = 13U, .app_index = 12U},
        {.occupied = true, .key = 14U, .app_index = 13U},
        {.occupied = true, .key = 15U, .app_index = 14U},
    }};
    Check(HallCoverCachePolicy::ReplacementIndex(kFullCache, 113U, 12U, 9U, 15U) == 3U,
          "an updated App must replace its stale cover when the visible cache is full");

    auto cache_with_empty_slot = kFullCache;
    cache_with_empty_slot[5].occupied = false;
    Check(HallCoverCachePolicy::ReplacementIndex(cache_with_empty_slot, 113U, 12U, 9U, 15U) == 5U,
          "an empty cover slot must be preferred over evicting stale data");

    Check(HallCoverCachePolicy::ReplacementIndex(kFullCache, 99U, 15U, 10U, 16U) == 0U,
          "a cover outside the current window must remain the fallback eviction candidate");
}

void RepeatedThrowMomentum() {
    Check(HallCarousel::InertiaVelocity(2000, 0U) == 2000,
          "interrupted inertia must expose its initial instantaneous velocity");
    const int32_t decayed = HallCarousel::InertiaVelocity(2000, 350U);
    Check(decayed >= 990 && decayed <= 995, "interrupted inertia must decay with the same exponential friction");
    const int32_t boosted = HallCarousel::BoostRepeatedThrow(1200, 2000, 350U);
    Check(boosted >= 1695 && boosted <= 1700, "a repeated throw in the same direction must retain momentum");
    Check(HallCarousel::BoostRepeatedThrow(-1200, 2000, 350U) == -1200,
          "a reversed throw must brake instead of inheriting opposite momentum");
    Check(HallCarousel::BoostRepeatedThrow(40, 2000, 20U) == 40,
          "a slow drag must not unexpectedly restart interrupted inertia");
    Check(HallCarousel::BoostRepeatedThrow(3800, 2000, 20U) == HallCarousel::kMaximumThrowVelocity,
          "repeated throws must remain bounded by the maximum velocity");
}

void HallLaunchBackgroundPolicy() {
    Check(PlanHallLaunchBackground(false, false) == HallLaunchBackgroundPlan::kCaptureVisibleHall &&
              PlanHallLaunchBackground(false, true) == HallLaunchBackgroundPlan::kCaptureVisibleHall,
          "a Hall without a running App may refresh its visible launch baseline");
    Check(PlanHallLaunchBackground(true, true) == HallLaunchBackgroundPlan::kReuseCleanBaseline,
          "an App switch must reuse the clean baseline instead of capturing the RUNNING card");
    Check(PlanHallLaunchBackground(true, false) == HallLaunchBackgroundPlan::kPrepareCleanBaseline,
          "an App switch without a cached baseline must render a clean fallback");
}

}  // namespace

int main() {
    CapacityAndGeometry();
    Square480Geometry();
    DragClampingAndDirection();
    VelocityAndFreeInertia();
    ContinuousIndicator();
    CardRevealOffset();
    BoundedCoverWindow();
    CoverCacheReplacementAfterAppUpdate();
    RepeatedThrowMomentum();
    HallLaunchBackgroundPolicy();
    std::cout << "hall_carousel tests passed: 10 cases\n";
    return 0;
}
