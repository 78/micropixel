#include "host/ui/lvgl/square_common/system_detail_ui.hpp"

#include <algorithm>

#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {

void SystemDetailUi::ResetActiveScreen() {
    power_animation_refresh_.Stop();
    root_ = nullptr;
    action_sink_ = nullptr;
    action_context_ = nullptr;
    power_switch_ = nullptr;
    power_timeout_ = nullptr;
    appearance_options_ = {};
    remote_control_scroll_ = nullptr;
    remote_control_scroll_offset_ = 0;
    remote_control_scroll_gesture_active_ = false;
    remote_control_render_pending_ = false;
    remote_control_off_confirmation_visible_ = false;
    app_management_selected_index_ = 0U;
    app_management_overlay_ = AppOverlay::kNone;
    app_management_overlay_root_ = nullptr;
    updating_ = false;
    active_screen_ = Screen::kNone;
}

void SystemDetailUi::ScrollEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    lv_obj_t* scroll = lv_event_get_current_target_obj(event);
    if (ui == nullptr || ui->root_ == nullptr || scroll == nullptr) {
        return;
    }
    if (ui->RemoteControlVisible() && scroll == ui->remote_control_scroll_) {
        ui->remote_control_scroll_offset_ = std::max<int32_t>(0, lv_obj_get_scroll_y(scroll));
        if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
            ui->remote_control_scroll_gesture_active_ = false;
            if (ui->remote_control_render_pending_) {
                ui->remote_control_render_pending_ = false;
                ui->QueueRemoteControlRender();
            }
        } else {
            lv_indev_t* indev = lv_indev_active();
            if (indev != nullptr && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                ui->remote_control_scroll_gesture_active_ = true;
            }
        }
    }
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(ui->root_));
}

}  // namespace micropixel::host_ui::lvgl::square_common
