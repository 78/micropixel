#pragma once

#include "lvgl.h"

namespace micropixel::host_ui::lvgl::square_common {

// Host-owned foreground affordance for the bottom-center suspend gesture.
// Callers hold the LVGL adapter lock for every public method.
class GuestGestureHintUi final {
   public:
    GuestGestureHintUi() = default;
    GuestGestureHintUi(const GuestGestureHintUi&) = delete;
    GuestGestureHintUi& operator=(const GuestGestureHintUi&) = delete;

    void ShowLocked(lv_display_t* display);
    void HideLocked();
    void RaiseLocked();
    [[nodiscard]] bool VisibleLocked() const;

   private:
    static void HideTimerCallback(lv_timer_t* timer);
    void HideObjectLocked();

    lv_obj_t* indicator_{};
    lv_timer_t* hide_timer_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
