#include <algorithm>

#include "esp_log.h"
#include "host/ui/lvgl/square_common/system_detail_ui.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui_internal.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
using system_detail_internal::Header;
using system_detail_internal::kAutoSleepTimeoutMinutes;
using system_detail_internal::Label;
using system_detail_internal::Panel;
using system_detail_internal::Scroll;
}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowPowerManagementLocked(
    lv_obj_t* root, const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    ResetActiveScreen();
    root_ = root;
    power_management_model_ = model;
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kPowerManagement;
    RenderPowerManagementLocked();
    return {};
}

void SystemDetailUi::RenderPowerManagementLocked() {
    if (!PowerManagementVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    Header(layout_, root_, "Power Management", "Idle and automatic sleep", PowerManagementBackEvent, this);
    lv_obj_t* content = Scroll(layout_, root_, ScrollEvent, this);

    lv_obj_t* automatic = Panel(layout_, content);
    lv_obj_set_flex_flow(automatic, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(automatic, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(automatic, 12, 0);
    lv_obj_t* automatic_text = square_common::CreateSystemColumn(automatic, 2);
    lv_obj_set_width(automatic_text, 0);
    lv_obj_set_flex_grow(automatic_text, 1);
    (void)Label(automatic_text, "Auto sleep", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    (void)Label(automatic_text, "Sleep when the device is idle", platform::lvgl::SystemFontRole::kSmall,
                theme::kSecondaryText);
    power_switch_ = lv_switch_create(automatic);
    lv_obj_set_size(power_switch_, 64, 36);
    lv_obj_add_event_cb(power_switch_, PowerManagementSwitchEvent, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* timeout = Panel(layout_, content);
    (void)Label(timeout, "Idle timeout", platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
    (void)Label(timeout, "Choose how long to wait", platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    power_timeout_ = lv_dropdown_create(timeout);
    lv_dropdown_set_options(power_timeout_, "1 minute\n5 minutes\n10 minutes\n30 minutes");
    lv_obj_set_size(power_timeout_, LV_PCT(100), layout_.control_height);
    lv_obj_set_style_text_font(power_timeout_,
                               platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kMedium), 0);
    lv_obj_set_style_text_font(
        power_timeout_, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kMedium), LV_PART_INDICATOR);
    lv_obj_add_event_cb(power_timeout_, PowerManagementTimeoutEvent, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t* note = Label(content, "External power pauses the idle timer. Unplugging starts a fresh countdown.",
                           layout_.detail_font, theme::kMutedText);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    UpdatePowerManagementLocked(power_management_model_);
    lv_obj_move_foreground(root_);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::UpdatePowerManagementLocked(const host_ui::PowerManagementModel& model) {
    if (!PowerManagementVisible() || power_switch_ == nullptr || power_timeout_ == nullptr) {
        return;
    }
    power_management_model_ = model;
    updating_ = true;
    const bool enabled = model.auto_sleep_timeout_minutes != 0U;
    if (enabled) {
        lv_obj_add_state(power_switch_, LV_STATE_CHECKED);
        lv_obj_remove_state(power_timeout_, LV_STATE_DISABLED);
        lv_obj_set_style_opa(power_timeout_, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_state(power_switch_, LV_STATE_CHECKED);
        lv_obj_add_state(power_timeout_, LV_STATE_DISABLED);
        lv_obj_set_style_opa(power_timeout_, LV_OPA_40, 0);
    }
    const uint8_t minutes = enabled ? model.auto_sleep_timeout_minutes : host_ui::kDefaultAutoSleepTimeoutMinutes;
    uint32_t selected = 1U;
    for (uint32_t index = 0U; index < kAutoSleepTimeoutMinutes.size(); ++index) {
        if (kAutoSleepTimeoutMinutes[index] == minutes) {
            selected = index;
            break;
        }
    }
    lv_dropdown_set_selected(power_timeout_, selected);
    updating_ = false;
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::LeavePowerManagement() {
    if (PowerManagementVisible()) {
        power_management_model_ = {};
        ResetActiveScreen();
    }
}

bool SystemDetailUi::PowerManagementVisible() const { return active_screen_ == Screen::kPowerManagement; }

void* SystemDetailUi::PowerManagementActionContext() const { return action_context_; }

void SystemDetailUi::PowerManagementBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kClosePowerManagement});
    }
}

void SystemDetailUi::PowerManagementSwitchEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->updating_ || ui->action_sink_ == nullptr) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(event);
    ui->power_animation_refresh_.Start(lv_obj_get_display(target));
    ui->action_sink_(ui->action_context_,
                     host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetAutoSleepEnabled,
                                             .value = lv_obj_has_state(target, LV_STATE_CHECKED) ? 1U : 0U});
}

void SystemDetailUi::PowerManagementTimeoutEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->updating_ || ui->action_sink_ == nullptr) {
        return;
    }
    const uint32_t selected = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (selected < kAutoSleepTimeoutMinutes.size()) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetAutoSleepTimeout,
                                                 .value = kAutoSleepTimeoutMinutes[selected]});
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
