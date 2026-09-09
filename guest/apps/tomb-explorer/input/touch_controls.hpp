#ifndef MICROPIXEL_APPS_TOMB_EXPLORER_INPUT_TOUCH_CONTROLS_HPP
#define MICROPIXEL_APPS_TOMB_EXPLORER_INPUT_TOUCH_CONTROLS_HPP

#include <stdint.h>

#include "apps/tomb-explorer/game/player.hpp"
#include "sdk/event.hpp"

namespace tomb::input {

// Left half of the panel: virtual stick, the first touch fixes its centre.
// Right half: dragging orbits (horizontal) and tilts (vertical) the camera; a
// short tap jumps. The function key also jumps.
class TouchControls final {
   public:
    void Initialize(int panel_width) { half_width_ = panel_width / 2; }

    // Touch coordinates are panel pixels.
    void OnTouch(const micropixel::TouchEvent& touch);
    void OnKey(const micropixel::KeyEvent& key);
    // Drains the deltas accumulated since the previous call.
    [[nodiscard]] game::Controls Consume();

    struct Overlay final {
        bool stick_active{};
        int origin_x{}, origin_y{};
        int x{}, y{};
    };
    [[nodiscard]] Overlay overlay() const {
        return {stick_.down, stick_.origin_x, stick_.origin_y, stick_.x, stick_.y};
    }

   private:
    struct Finger final {
        bool down{};
        uint32_t id{};
        int x{}, y{};
        int origin_x{}, origin_y{};
        int travel{};
        uint64_t down_at_us{};
    };

    [[nodiscard]] static float Deflection(int delta);

    int half_width_{240};
    Finger stick_{};
    Finger look_{};
    float orbit_accum_{};
    float tilt_accum_{};
    bool jump_pending_{};
};

}  // namespace tomb::input

#endif
