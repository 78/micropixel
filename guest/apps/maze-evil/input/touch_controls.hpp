#ifndef MICROPIXEL_APPS_MAZE_BREAK_INPUT_TOUCH_CONTROLS_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_INPUT_TOUCH_CONTROLS_HPP

#include <stdint.h>

#include "apps/maze-evil/game/world.hpp"
#include "sdk/event.hpp"

namespace maze_break::input {

// Touch and key input, fed from the SDK event stream.
//
// Left half: virtual stick. The first finger down defines the centre; forward
// and strafe come from its displacement.
// Right half in touch mode: dragging turns, a short tap fires once, and a
// finger held still fires repeatedly. In motion mode turning comes from the
// IMU, so the whole right half is a plain fire button.
// The function key (kConfirm) fires while held.
class TouchControls final {
   public:
    void Initialize(int view_width) { half_width_ = view_width / 2; }
    void SetMotionMode(bool enabled) { motion_mode_ = enabled; }

    void OnTouch(const micropixel::TouchEvent& touch);
    void OnKey(const micropixel::KeyEvent& key);

    // Drains the per-frame deltas; `now_us` decides whether a held finger has
    // turned into repeat fire.
    [[nodiscard]] game::Controls Consume(uint64_t now_us);

    // True while the function key has been held for longer than
    // `hold_us`; used by the caller to trigger IMU recalibration.
    [[nodiscard]] bool KeyHeldFor(uint64_t now_us, uint64_t hold_us) const {
        return key_down_ && key_down_at_us_ != 0U && now_us - key_down_at_us_ > hold_us;
    }

    // Virtual stick geometry for the on-screen overlay, in panel pixels.
    struct Overlay {
        bool stick_active{};
        int stick_origin_x{}, stick_origin_y{};
        int stick_x{}, stick_y{};
    };
    [[nodiscard]] Overlay overlay() const {  // NOLINT(readability-identifier-naming)
        return Overlay{stick_.down, stick_.origin_x, stick_.origin_y, stick_.x, stick_.y};
    }

   private:
    struct Finger {
        bool down{};
        uint32_t id{};
        int x{}, y{};
        int origin_x{}, origin_y{};
        uint64_t down_at_us{};
        int travel{};
    };

    [[nodiscard]] static float Deflection(int delta);
    void Release(Finger& finger, uint64_t now_us);

    int half_width_{240};
    bool motion_mode_{};
    Finger stick_{};
    Finger look_{};
    float turn_accum_{};
    bool tap_fired_{};
    bool key_down_{};
    uint64_t key_down_at_us_{};
};

}  // namespace maze_break::input

#endif
