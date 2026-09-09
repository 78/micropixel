#include "apps/tomb-explorer/input/touch_controls.hpp"

namespace tomb::input {
namespace {

constexpr int kStickRadius = 70;  // panel pixels for full deflection
constexpr int kStickDeadzone = 8;
constexpr float kOrbitPerPixel = 0.008F;  // radians
constexpr float kTiltPerPixel = 0.005F;
constexpr int kTapTravel = 12;
constexpr uint64_t kTapMaxUs = 240'000U;

int Abs(int value) { return value < 0 ? -value : value; }

}  // namespace

float TouchControls::Deflection(int delta) {
    if (Abs(delta) < kStickDeadzone) return 0.0F;
    const float value = static_cast<float>(delta) / static_cast<float>(kStickRadius);
    return value > 1.0F ? 1.0F : (value < -1.0F ? -1.0F : value);
}

void TouchControls::OnTouch(const micropixel::TouchEvent& touch) {
    const uint64_t now_us = touch.timestamp().microseconds();
    const uint32_t id = touch.id();
    const int x = static_cast<int>(touch.x());
    const int y = static_cast<int>(touch.y());
    switch (touch.phase()) {
        case micropixel::TouchPhase::kDown: {
            Finger& finger = x < half_width_ ? stick_ : look_;
            if (finger.down) return;  // one finger per half
            finger = Finger{true, id, x, y, x, y, 0, now_us};
            return;
        }
        case micropixel::TouchPhase::kMove:
            if (stick_.down && stick_.id == id) {
                stick_.x = x;
                stick_.y = y;
            } else if (look_.down && look_.id == id) {
                const int dx = x - look_.x;
                const int dy = y - look_.y;
                look_.travel += Abs(dx) + Abs(dy);
                orbit_accum_ += static_cast<float>(dx) * kOrbitPerPixel;
                tilt_accum_ -= static_cast<float>(dy) * kTiltPerPixel;
                look_.x = x;
                look_.y = y;
            }
            return;
        case micropixel::TouchPhase::kUp:
        case micropixel::TouchPhase::kCancel:
            if (stick_.down && stick_.id == id) {
                stick_.down = false;
            } else if (look_.down && look_.id == id) {
                if (touch.phase() == micropixel::TouchPhase::kUp && look_.travel < kTapTravel &&
                    now_us - look_.down_at_us < kTapMaxUs) {
                    jump_pending_ = true;
                }
                look_.down = false;
            }
            return;
    }
}

void TouchControls::OnKey(const micropixel::KeyEvent& key) {
    if (key.code() == micropixel::KeyCode::kConfirm && key.phase() == micropixel::KeyPhase::kDown) {
        jump_pending_ = true;
    }
}

game::Controls TouchControls::Consume() {
    game::Controls controls{};
    if (stick_.down) {
        controls.forward = -Deflection(stick_.y - stick_.origin_y);
        controls.strafe = Deflection(stick_.x - stick_.origin_x);
    }
    controls.orbit = orbit_accum_;
    controls.tilt = tilt_accum_;
    controls.jump = jump_pending_;
    orbit_accum_ = 0.0F;
    tilt_accum_ = 0.0F;
    jump_pending_ = false;
    return controls;
}

}  // namespace tomb::input
