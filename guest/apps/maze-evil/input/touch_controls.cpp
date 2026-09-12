#include "apps/maze-evil/input/touch_controls.hpp"

namespace maze_break::input {
namespace {

constexpr int kStickRadius = 70;  // panel pixels for full deflection
constexpr int kStickDeadzone = 10;
constexpr float kTurnPerPixel = 0.0075F;  // radians
int Abs(int value) { return value < 0 ? -value : value; }

}  // namespace

float TouchControls::Deflection(int delta) {
    if (Abs(delta) < kStickDeadzone) {
        return 0.0F;
    }
    const float value = static_cast<float>(delta) / kStickRadius;
    return value > 1.0F ? 1.0F : (value < -1.0F ? -1.0F : value);
}

bool TouchControls::HitsFire(int x, int y) const {
    const int dx = x - fire_x_;
    const int dy = y - fire_y_;
    return dx * dx + dy * dy <= fire_hit_radius_ * fire_hit_radius_;
}

void TouchControls::OnTouch(const micropixel::TouchEvent& touch) {
    const uint32_t id = touch.id();
    const int x = static_cast<int>(touch.x());
    const int y = static_cast<int>(touch.y());
    switch (touch.phase()) {
        case micropixel::TouchPhase::kDown: {
            if ((stick_.down && stick_.id == id) || (look_.down && look_.id == id) || (fire_.down && fire_.id == id)) {
                return;
            }
            Finger& finger = HitsFire(x, y) ? fire_ : (x < half_width_ ? stick_ : look_);
            if (finger.down) {
                return;  // one finger per control
            }
            finger = Finger{.down = true, .id = id, .x = x, .y = y, .origin_x = x, .origin_y = y};
            if (&finger == &fire_) {
                tap_fired_ = true;
            }
            return;
        }
        case micropixel::TouchPhase::kMove: {
            if (stick_.down && stick_.id == id) {
                stick_.x = x;
                stick_.y = y;
            } else if (look_.down && look_.id == id) {
                const int dx = x - look_.x;
                turn_accum_ += dx * kTurnPerPixel;
                look_.x = x;
                look_.y = y;
            }
            return;
        }
        case micropixel::TouchPhase::kUp:
        case micropixel::TouchPhase::kCancel:
            if (stick_.down && stick_.id == id) {
                stick_.down = false;
            } else if (look_.down && look_.id == id) {
                look_.down = false;
            } else if (fire_.down && fire_.id == id) {
                fire_.down = false;
                if (touch.phase() == micropixel::TouchPhase::kCancel) {
                    tap_fired_ = false;
                }
            }
            return;
    }
}

void TouchControls::OnKey(const micropixel::KeyEvent& key) {
    if (key.code() != micropixel::KeyCode::kConfirm) {
        return;
    }
    const bool down = key.phase() == micropixel::KeyPhase::kDown || key.phase() == micropixel::KeyPhase::kRepeat;
    if (down && !key_down_) {
        key_down_at_us_ = key.timestamp().microseconds();
    }
    key_down_ = down;
}

game::Controls TouchControls::Consume(uint64_t /*now_us*/) {
    game::Controls controls{};
    if (stick_.down) {
        controls.forward = -Deflection(stick_.y - stick_.origin_y);
        controls.strafe = Deflection(stick_.x - stick_.origin_x);
    }
    controls.turn = turn_accum_;
    turn_accum_ = 0.0F;

    controls.fire = key_down_ || fire_.down || tap_fired_;
    tap_fired_ = false;
    return controls;
}

}  // namespace maze_break::input
