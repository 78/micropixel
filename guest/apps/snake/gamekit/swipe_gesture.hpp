#ifndef MICROPIXEL_SNAKE_GAMEKIT_SWIPE_GESTURE_HPP
#define MICROPIXEL_SNAKE_GAMEKIT_SWIPE_GESTURE_HPP

#include <stdint.h>

#include "sdk/event.hpp"

namespace snake::gamekit {

enum class GestureKind : uint8_t {
    kNone,
    kTap,
    kSwipe,
};

struct Gesture final {
    GestureKind kind{GestureKind::kNone};
    int32_t x{};
    int32_t y{};
    int32_t dx{};
    int32_t dy{};
};

// Touch events use the Guest logical coordinate space. Convert a desired
// physical travel distance with ceiling division so a gesture never triggers
// before the requested number of display pixels has been crossed.
[[nodiscard]] constexpr int32_t ScalePhysicalThreshold(uint32_t physical_pixels, uint32_t logical_extent,
                                                       uint32_t physical_extent) {
    if (physical_extent == 0U) {
        return static_cast<int32_t>(physical_pixels);
    }
    return static_cast<int32_t>((static_cast<uint64_t>(physical_pixels) * logical_extent + physical_extent - 1U) /
                                physical_extent);
}

// Tracks one touch and emits incremental swipes while the finger remains down.
// This preserves Snake's held-swipe behavior without leaking gesture state into
// the game controller.
class SwipeGesture final {
   public:
    constexpr SwipeGesture(int32_t threshold_x, int32_t threshold_y)
        : threshold_x_(threshold_x), threshold_y_(threshold_y) {}

    [[nodiscard]] Gesture Update(const micropixel::TouchEvent& touch) {
        if (touch.phase() == micropixel::TouchPhase::kDown) {
            active_ = true;
            had_swipe_ = false;
            touch_id_ = touch.id();
            anchor_x_ = touch.x();
            anchor_y_ = touch.y();
            return {};
        }
        if (!active_ || touch.id() != touch_id_) {
            return {};
        }

        const int32_t dx = static_cast<int32_t>(touch.x()) - anchor_x_;
        const int32_t dy = static_cast<int32_t>(touch.y()) - anchor_y_;
        if (touch.phase() == micropixel::TouchPhase::kMove) {
            return SwipeIfReady(touch.x(), touch.y(), dx, dy);
        }
        if (touch.phase() != micropixel::TouchPhase::kUp && touch.phase() != micropixel::TouchPhase::kCancel) {
            return {};
        }

        active_ = false;
        if (touch.phase() == micropixel::TouchPhase::kCancel || had_swipe_) {
            return {};
        }
        if (Absolute(dx) < kTapThresholdPx && Absolute(dy) < kTapThresholdPx) {
            return Gesture{GestureKind::kTap, touch.x(), touch.y(), dx, dy};
        }
        return SwipeIfReady(touch.x(), touch.y(), dx, dy);
    }

   private:
    static constexpr int32_t kTapThresholdPx = 30;

    [[nodiscard]] static int32_t Absolute(int32_t value) { return value < 0 ? -value : value; }

    [[nodiscard]] Gesture SwipeIfReady(int32_t x, int32_t y, int32_t dx, int32_t dy) {
        if (Absolute(dx) < threshold_x_ && Absolute(dy) < threshold_y_) {
            return {};
        }
        had_swipe_ = true;
        anchor_x_ = x;
        anchor_y_ = y;
        return Gesture{GestureKind::kSwipe, x, y, dx, dy};
    }

    bool active_{};
    bool had_swipe_{};
    uint32_t touch_id_{};
    int32_t anchor_x_{};
    int32_t anchor_y_{};
    int32_t threshold_x_{};
    int32_t threshold_y_{};
};

}  // namespace snake::gamekit

#endif
