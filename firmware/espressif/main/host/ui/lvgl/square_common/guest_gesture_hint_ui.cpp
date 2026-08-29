#include "host/ui/lvgl/square_common/guest_gesture_hint_ui.hpp"

#include <algorithm>

#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

constexpr uint32_t kVisibleDurationMs = 3000U;
constexpr uint8_t kIndicatorOpacity = 176U;

}  // namespace

void GuestGestureHintUi::ShowLocked(lv_display_t* display) {
    if (display == nullptr) {
        return;
    }
    if (indicator_ == nullptr) {
        const int32_t display_width = lv_display_get_horizontal_resolution(display);
        const int32_t indicator_width = std::max<int32_t>(80, display_width / 5);
        const int32_t indicator_height = std::max<int32_t>(6, display_width / 90);
        const int32_t bottom_margin = std::max<int32_t>(10, display_width / 48);
        indicator_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(indicator_, indicator_width, indicator_height);
        lv_obj_align(indicator_, LV_ALIGN_BOTTOM_MID, 0, -bottom_margin);
        lv_obj_set_style_pad_all(indicator_, 0, 0);
        lv_obj_set_style_border_width(indicator_, 0, 0);
        lv_obj_set_style_radius(indicator_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(indicator_, kIndicatorOpacity, 0);
        lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_color(indicator_, lv_color_hex(theme::kOverlayText), 0);
    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
    RaiseLocked();
    if (hide_timer_ == nullptr) {
        hide_timer_ = lv_timer_create(HideTimerCallback, kVisibleDurationMs, this);
    } else {
        lv_timer_set_period(hide_timer_, kVisibleDurationMs);
        lv_timer_reset(hide_timer_);
        lv_timer_resume(hide_timer_);
    }
    platform::lvgl::RequestDisplayRefresh(display);
}

void GuestGestureHintUi::HideLocked() {
    if (hide_timer_ != nullptr) {
        lv_timer_delete(hide_timer_);
        hide_timer_ = nullptr;
    }
    HideObjectLocked();
}

void GuestGestureHintUi::RaiseLocked() {
    if (VisibleLocked()) {
        lv_obj_move_foreground(indicator_);
    }
}

bool GuestGestureHintUi::VisibleLocked() const {
    return indicator_ != nullptr && !lv_obj_has_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
}

void GuestGestureHintUi::HideTimerCallback(lv_timer_t* timer) {
    auto* ui = static_cast<GuestGestureHintUi*>(lv_timer_get_user_data(timer));
    if (ui == nullptr) {
        lv_timer_delete(timer);
        return;
    }
    ui->hide_timer_ = nullptr;
    lv_timer_delete(timer);
    ui->HideObjectLocked();
}

void GuestGestureHintUi::HideObjectLocked() {
    if (!VisibleLocked()) {
        return;
    }
    lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(indicator_));
}

}  // namespace micropixel::host_ui::lvgl::square_common
