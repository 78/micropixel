#include "apps/maze-break/input/touch_controls.hpp"

namespace maze_break::input {
namespace {

constexpr int kStickRadius = 70;  // panel pixels for full deflection
constexpr int kStickDeadzone = 10;
constexpr float kTurnPerPixel = 0.0075F;  // radians
constexpr int kTapTravel = 14;
constexpr uint64_t kTapMaxUs = 260'000U;
constexpr uint64_t kHoldFireUs = 320'000U;

int Abs(int value) { return value < 0 ? -value : value; }

}  // namespace

float TouchControls::Deflection(int delta) {
    if (Abs(delta) < kStickDeadzone) {
        return 0.0F;
    }
    const float value = static_cast<float>(delta) / kStickRadius;
    return value > 1.0F ? 1.0F : (value < -1.0F ? -1.0F : value);
}

void TouchControls::Release(Finger& finger, uint64_t now_us) {
    if (&finger == &look_ && finger.down && !motion_mode_ && finger.travel < kTapTravel &&
        now_us - finger.down_at_us < kTapMaxUs) {
        tap_fired_ = true;
    }
    finger.down = false;
}

void TouchControls::OnTouch(const micropixel::TouchEvent& touch) {
    const uint64_t now_us = touch.timestamp().microseconds();
    const uint32_t id = touch.id();
    const int x = static_cast<int>(touch.x());
    const int y = static_cast<int>(touch.y());
    switch (touch.phase()) {
        case micropixel::TouchPhase::kDown: {
            Finger& finger = x < half_width_ ? stick_ : look_;
            if (finger.down) {
                return;  // one finger per half
            }
            finger = Finger{.down = true,
                            .id = id,
                            .x = x,
                            .y = y,
                            .origin_x = x,
                            .origin_y = y,
                            .down_at_us = now_us,
                            .travel = 0};
            return;
        }
        case micropixel::TouchPhase::kMove: {
            if (stick_.down && stick_.id == id) {
                stick_.x = x;
                stick_.y = y;
            } else if (look_.down && look_.id == id) {
                const int dx = x - look_.x;
                const int dy = y - look_.y;
                look_.travel += Abs(dx) + Abs(dy);
                if (!motion_mode_) {
                    turn_accum_ += dx * kTurnPerPixel;
                }
                look_.x = x;
                look_.y = y;
            }
            return;
        }
        case micropixel::TouchPhase::kUp:
        case micropixel::TouchPhase::kCancel:
            if (stick_.down && stick_.id == id) {
                Release(stick_, now_us);
            } else if (look_.down && look_.id == id) {
                Release(look_, now_us);
            }
            return;
    }
}

void TouchControls::OnKey(const micropixel::KeyEvent& key) {
    if (key.code() != micropixel::KeyCode::kConfirm) {
        return;
    }
    const bool down = key.phase() == micropixel::KeyPhase::kDown;
    if (down && !key_down_) {
        key_down_at_us_ = key.timestamp().microseconds();
    }
    key_down_ = down;
}

game::Controls TouchControls::Consume(uint64_t now_us) {
    game::Controls controls{};
    if (stick_.down) {
        controls.forward = -Deflection(stick_.y - stick_.origin_y);
        controls.strafe = Deflection(stick_.x - stick_.origin_x);
    }
    controls.turn = turn_accum_;
    turn_accum_ = 0.0F;

    bool fire = key_down_;
    if (look_.down) {
        if (motion_mode_) {
            fire = true;
        } else if (look_.travel < kTapTravel && now_us - look_.down_at_us > kHoldFireUs) {
            fire = true;
        }
    }
    controls.fire = fire || tap_fired_;
    tap_fired_ = false;
    return controls;
}

}  // namespace maze_break::input
