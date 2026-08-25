#include <cstdlib>
#include <iostream>

#include "platform/metalio-claw4/hall_carousel.hpp"

namespace {

using micropixel::platform::metalio_claw4::HallCarousel;
using micropixel::platform::metalio_claw4::HallVelocityTracker;

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

}  // namespace

int main() {
    CapacityAndGeometry();
    DragClampingAndDirection();
    VelocityAndFreeInertia();
    ContinuousIndicator();
    BoundedCoverWindow();
    RepeatedThrowMomentum();
    std::cout << "hall_carousel tests passed: 6 cases\n";
    return 0;
}
