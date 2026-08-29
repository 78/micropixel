#include "esp_log.h"
#include "host/ui/lvgl/square_common/system_detail_ui.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui_internal.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
using system_detail_internal::Button;
using system_detail_internal::CreateActionSheet;
using system_detail_internal::Header;
using system_detail_internal::InformationRow;
using system_detail_internal::kTag;
using system_detail_internal::Label;
using system_detail_internal::Panel;
using system_detail_internal::RemoteState;
using system_detail_internal::RemoteStateColor;
using system_detail_internal::Scroll;
}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowRemoteControlLocked(
    lv_obj_t* root, const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    ResetActiveScreen();
    root_ = root;
    remote_control_model_ = model;
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kRemoteControl;
    RenderRemoteControlLocked();
    return {};
}

void SystemDetailUi::RenderRemoteControlLocked() {
    if (!RemoteControlVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    Header(layout_, root_, "Remote Control", "Service and temporary connection code", RemoteControlBackEvent, this);
    lv_obj_t* scroll = Scroll(layout_, root_, ScrollEvent, this);

    lv_obj_t* status = Panel(layout_, scroll, 0);
    lv_obj_t* status_heading = square_common::CreateSystemColumn(status, 0);
    lv_obj_set_height(status_heading, layout_.row_height);
    lv_obj_set_flex_flow(status_heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_heading, 12, 0);
    lv_obj_t* dot = lv_obj_create(status_heading);
    lv_obj_set_size(dot, 11, 11);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(RemoteStateColor(remote_control_model_.connection_state)), 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    (void)Label(status_heading, RemoteState(remote_control_model_.connection_state),
                platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    InformationRow(layout_, status, "Service",
                   remote_control_model_.service[0] != '\0' ? remote_control_model_.service.data() : "Not configured");

    lv_obj_t* pairing = Panel(layout_, scroll);
    (void)Label(pairing, "Connect this device", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    if (remote_control_model_.pairing_code_available) {
        lv_obj_t* code = Label(pairing, remote_control_model_.pairing_code.data(),
                               platform::lvgl::SystemFontRole::kTitle, theme::kPositive);
        lv_obj_set_width(code, LV_PCT(100));
        lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
        char expiry[48]{};
        std::snprintf(expiry, sizeof(expiry), "Expires in %02" PRIu32 ":%02" PRIu32,
                      remote_control_model_.pairing_expires_seconds / 60U,
                      remote_control_model_.pairing_expires_seconds % 60U);
        lv_obj_t* expiry_label = Label(pairing, expiry, platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
        lv_obj_set_width(expiry_label, LV_PCT(100));
        lv_obj_set_style_text_align(expiry_label, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        const bool available =
            remote_control_model_.enabled &&
            remote_control_model_.connection_state == host_ui::RemoteControlConnectionState::kConnected &&
            !remote_control_model_.pairing_code_pending;
        const char* text = remote_control_model_.pairing_code_pending ? "Getting connection code..."
                           : available                                ? "Generate Connection Code"
                                                                      : "Control service unavailable";
        lv_obj_t* generate = Button(layout_, pairing, text, available ? theme::kPositive : theme::kSecondaryText);
        if (available) {
            lv_obj_add_event_cb(generate, RemoteControlPairingEvent, LV_EVENT_SHORT_CLICKED, this);
        } else {
            lv_obj_remove_flag(generate, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    lv_obj_t* note = Label(pairing, "Codes are single-use and expire after 5 minutes",
                           platform::lvgl::SystemFontRole::kSmall, theme::kMutedText);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* toggle =
        Button(layout_, scroll, remote_control_model_.enabled ? "Turn Remote Control Off" : "Turn Remote Control On",
               remote_control_model_.enabled ? theme::kDanger : theme::kAccent);
    lv_obj_add_event_cb(toggle, RemoteControlToggleEvent, LV_EVENT_SHORT_CLICKED, this);
    if (remote_control_off_confirmation_visible_) {
        DrawRemoteControlOffConfirmationLocked();
    }
    lv_obj_move_foreground(root_);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::UpdateRemoteControlLocked(const host_ui::RemoteControlModel& model) {
    if (RemoteControlVisible()) {
        remote_control_model_ = model;
        if (!model.enabled) {
            remote_control_off_confirmation_visible_ = false;
        }
        RenderRemoteControlLocked();
    }
}

void SystemDetailUi::LeaveRemoteControl() {
    if (RemoteControlVisible()) {
        remote_control_model_ = {};
        ResetActiveScreen();
    }
}

bool SystemDetailUi::RemoteControlVisible() const { return active_screen_ == Screen::kRemoteControl; }

void* SystemDetailUi::RemoteControlActionContext() const { return action_context_; }

void SystemDetailUi::RemoteControlBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseRemoteControl});
    }
}

void SystemDetailUi::RemoteControlToggleEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->action_sink_ == nullptr) {
        return;
    }
    if (ui->remote_control_model_.enabled) {
        ui->remote_control_off_confirmation_visible_ = true;
        ui->QueueRemoteControlRender();
        return;
    }
    ui->action_sink_(
        ui->action_context_,
        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetRemoteControlEnabled, .value = 1U});
}

void SystemDetailUi::RemoteControlPairingEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(
            ui->action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kGenerateRemoteControlPairingCode});
    }
}

void SystemDetailUi::RemoteControlConfirmationCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->remote_control_off_confirmation_visible_ = false;
        ui->QueueRemoteControlRender();
    }
}

void SystemDetailUi::RemoteControlConfirmOffEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->action_sink_ == nullptr || !ui->remote_control_model_.enabled) {
        return;
    }
    ui->remote_control_off_confirmation_visible_ = false;
    ui->action_sink_(
        ui->action_context_,
        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetRemoteControlEnabled, .value = 0U});
}

void SystemDetailUi::RemoteControlRenderAsync(void* context) {
    auto* ui = static_cast<SystemDetailUi*>(context);
    if (ui != nullptr && ui->RemoteControlVisible()) {
        ui->RenderRemoteControlLocked();
    }
}

void SystemDetailUi::QueueRemoteControlRender() {
    if (lv_async_call(RemoteControlRenderAsync, this) != LV_RESULT_OK) {
        ESP_LOGW(kTag, "failed to queue Remote Control render");
    }
}

void SystemDetailUi::DrawRemoteControlOffConfirmationLocked() {
    lv_obj_t* sheet =
        CreateActionSheet(layout_, root_, RemoteControlConfirmationCancelEvent, this, theme::kDangerBorder);
    (void)Label(sheet, "Turn Off Remote Control?", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    lv_obj_t* detail = Label(sheet, "Remote access and active connection codes will stop.",
                             platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* turn_off = Button(layout_, sheet, "Turn Off", theme::kDanger);
    lv_obj_add_event_cb(turn_off, RemoteControlConfirmOffEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel");
    lv_obj_add_event_cb(cancel, RemoteControlConfirmationCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

}  // namespace micropixel::host_ui::lvgl::square_common
