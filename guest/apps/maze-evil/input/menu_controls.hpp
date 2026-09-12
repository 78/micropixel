// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_APPS_MAZE_BREAK_INPUT_MENU_CONTROLS_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_INPUT_MENU_CONTROLS_HPP

#include "sdk/event.hpp"

namespace maze_break::input {

// Menu confirmation needs a fresh press and release. Reset on page transitions
// so a gameplay contact or a cancelled gesture cannot confirm another page.
class MenuControls final {
   public:
    bool OnTouch(const micropixel::TouchEvent& touch) {
        if (touch.phase() == micropixel::TouchPhase::kDown && !touch_down_) {
            touch_down_ = true;
            touch_id_ = touch.id();
        } else if (touch_down_ && touch.id() == touch_id_ &&
                   (touch.phase() == micropixel::TouchPhase::kUp || touch.phase() == micropixel::TouchPhase::kCancel)) {
            touch_down_ = false;
            return touch.phase() == micropixel::TouchPhase::kUp;
        }
        return false;
    }

    bool OnKey(const micropixel::KeyEvent& key) {
        if (key.phase() == micropixel::KeyPhase::kDown && !key_down_) {
            key_down_ = true;
            key_code_ = key.code();
        } else if (key_down_ && key.code() == key_code_ &&
                   (key.phase() == micropixel::KeyPhase::kUp || key.phase() == micropixel::KeyPhase::kCancel)) {
            key_down_ = false;
            return key.phase() == micropixel::KeyPhase::kUp;
        }
        return false;
    }

   private:
    bool touch_down_{};
    uint32_t touch_id_{};
    bool key_down_{};
    micropixel::KeyCode key_code_{};
};

}  // namespace maze_break::input

#endif
