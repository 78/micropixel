#ifndef MICROPIXEL_APPS_MAZE_BREAK_INPUT_TOUCH_CONTROLS_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_INPUT_TOUCH_CONTROLS_HPP

#include <stdint.h>

#include "apps/maze-evil/game/world.hpp"
#include "sdk/event.hpp"

namespace maze_break::input {

// Left: floating movement stick. Right: drag to turn. Middle-right: padded fire
// button. Each contact keeps its role until release, even across region boundaries.
class TouchControls final {
   public:
    void Initialize(int view_width, int view_height) {
        half_width_ = view_width / 2;
        const int unit = view_width < view_height ? view_width : view_height;
        fire_radius_ = unit / 10;
        fire_hit_radius_ = unit / 8;
        fire_x_ = view_width - unit / 8;
        fire_y_ = view_height / 2;
    }

    void OnTouch(const micropixel::TouchEvent& touch);
    void OnKey(const micropixel::KeyEvent& key);

    // Drains per-frame turn and fire presses.
    [[nodiscard]] game::Controls Consume(uint64_t now_us);

    // True while the function key has been held for longer than
    // `hold_us`; used by the caller to trigger IMU recalibration.
    [[nodiscard]] bool KeyHeldFor(uint64_t now_us, uint64_t hold_us) const {
        return key_down_ && key_down_at_us_ != 0U && now_us - key_down_at_us_ > hold_us;
    }

    // Virtual stick geometry for the on-screen overlay, in panel pixels.
    // The App maps SDK logical touch coordinates into this space first.
    struct Overlay {
        bool stick_active{};
        int stick_origin_x{}, stick_origin_y{};
        int stick_x{}, stick_y{};
        int fire_x{}, fire_y{}, fire_radius{};
        bool fire_active{};
    };
    [[nodiscard]] Overlay overlay() const {  // NOLINT(readability-identifier-naming)
        return Overlay{stick_.down, stick_.origin_x, stick_.origin_y, stick_.x,  stick_.y,
                       fire_x_,     fire_y_,         fire_radius_,    fire_.down};
    }

   private:
    struct Finger {
        bool down{};
        uint32_t id{};
        int x{}, y{};
        int origin_x{}, origin_y{};
    };

    [[nodiscard]] static float Deflection(int delta);
    [[nodiscard]] bool HitsFire(int x, int y) const;

    int half_width_{240};
    int fire_x_{420}, fire_y_{240}, fire_radius_{48}, fire_hit_radius_{60};
    Finger stick_{};
    Finger look_{};
    Finger fire_{};
    float turn_accum_{};
    bool tap_fired_{};
    bool key_down_{};
    uint64_t key_down_at_us_{};
};

}  // namespace maze_break::input

#endif
