#include "host/ui/lvgl/square_common/system_detail_ui.hpp"

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
    remote_control_off_confirmation_visible_ = false;
    app_management_selected_index_ = 0U;
    app_management_overlay_ = AppOverlay::kNone;
    app_management_overlay_root_ = nullptr;
    updating_ = false;
    active_screen_ = Screen::kNone;
}

void SystemDetailUi::ScrollEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->root_ != nullptr) {
        platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(ui->root_));
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
