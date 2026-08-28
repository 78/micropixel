#include "platform/lvgl/ui/square_common/system_detail_ui.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl::square_common {
namespace {

constexpr char kTag[] = "system_details";
constexpr std::array<uint8_t, 4U> kAutoSleepTimeoutMinutes{1U, 5U, 10U, 30U};

const char* DisplayText(const char* text, const char* fallback) {
    return text != nullptr && text[0] != '\0' ? text : fallback;
}

bool FirmwareUpdateInProgress(host_ui::FirmwareUpdateState state) {
    return state == host_ui::FirmwareUpdateState::kDownloading || state == host_ui::FirmwareUpdateState::kVerifying ||
           state == host_ui::FirmwareUpdateState::kInstalling;
}

const char* FirmwareUpdateStageText(host_ui::FirmwareUpdateState state) {
    switch (state) {
        case host_ui::FirmwareUpdateState::kChecking:
            return "Checking for update";
        case host_ui::FirmwareUpdateState::kDownloading:
            return "Downloading firmware";
        case host_ui::FirmwareUpdateState::kVerifying:
            return "Verifying package";
        case host_ui::FirmwareUpdateState::kInstalling:
            return "Installing firmware";
        case host_ui::FirmwareUpdateState::kCurrent:
            return "Firmware is up to date";
        case host_ui::FirmwareUpdateState::kAvailable:
            return "Ready to update";
        case host_ui::FirmwareUpdateState::kFailed:
            return "Update failed";
        case host_ui::FirmwareUpdateState::kUnknown:
            return "Firmware update";
    }
    return "Firmware update";
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, SystemFontRole role, uint32_t color) {
    return square_common::CreateSystemLabel(parent, text, role, color);
}

lv_obj_t* Panel(const SystemPageLayout& layout, lv_obj_t* parent, int32_t gap = -1) {
    return CreateSystemPanel(parent, layout, gap);
}

lv_obj_t* Button(const SystemPageLayout& layout, lv_obj_t* parent, const char* text, uint32_t color = 0xf2f7ffU) {
    return CreateSystemActionButton(parent, layout, text, color);
}

void Header(const SystemPageLayout& layout, lv_obj_t* root, const char* title, const char* subtitle,
            lv_event_cb_t back_event, void* context) {
    (void)CreateSystemHeader(root, layout, title, subtitle, back_event, context);
}

lv_obj_t* Scroll(const SystemPageLayout& layout, lv_obj_t* root, lv_event_cb_t event, void* context) {
    return CreateSystemScrollColumn(root, layout, event, context);
}

void SectionLabel(lv_obj_t* parent, const char* text) { (void)Label(parent, text, SystemFontRole::kSmall, 0x748aa5U); }

void InformationRow(const SystemPageLayout& layout, lv_obj_t* parent, const char* name, const char* value) {
    (void)CreateSystemInformationRow(parent, layout, name, value);
}

void FormatSize(uint32_t kib, char* output, size_t capacity) {
    if (kib >= 1024U) {
        const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(kib) * 10U + 512U) / 1024U);
        std::snprintf(output, capacity, "%" PRIu32 ".%" PRIu32 " MB", tenths / 10U, tenths % 10U);
    } else {
        std::snprintf(output, capacity, "%" PRIu32 " KB", kib);
    }
}

const char* RemoteState(host_ui::RemoteControlConnectionState state) {
    switch (state) {
        case host_ui::RemoteControlConnectionState::kDisabled:
            return "Off";
        case host_ui::RemoteControlConnectionState::kWaitingForNetwork:
            return "Waiting for network";
        case host_ui::RemoteControlConnectionState::kConnecting:
            return "Connecting";
        case host_ui::RemoteControlConnectionState::kConnected:
            return "Connected";
        case host_ui::RemoteControlConnectionState::kBackoff:
            return "Retrying";
        case host_ui::RemoteControlConnectionState::kAuthenticationError:
            return "Authentication error";
    }
    return "Unknown";
}

uint32_t RemoteStateColor(host_ui::RemoteControlConnectionState state) {
    switch (state) {
        case host_ui::RemoteControlConnectionState::kConnected:
            return 0xc5f36dU;
        case host_ui::RemoteControlConnectionState::kWaitingForNetwork:
        case host_ui::RemoteControlConnectionState::kConnecting:
            return 0x69a7ffU;
        case host_ui::RemoteControlConnectionState::kBackoff:
            return 0xf4c86aU;
        case host_ui::RemoteControlConnectionState::kAuthenticationError:
            return 0xff6b74U;
        case host_ui::RemoteControlConnectionState::kDisabled:
            return 0x748aa5U;
    }
    return 0x748aa5U;
}

lv_obj_t* CreateOverlay(lv_obj_t* root, lv_event_cb_t cancel_event, void* context) {
    lv_obj_t* overlay = lv_obj_create(root);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x020711U), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, cancel_event, LV_EVENT_SHORT_CLICKED, context);
    return overlay;
}

lv_obj_t* CreateActionSheet(const SystemPageLayout& layout, lv_obj_t* root, lv_event_cb_t cancel_event, void* context,
                            uint32_t border_color = 0x42607fU, lv_obj_t** overlay_out = nullptr) {
    lv_obj_t* overlay = CreateOverlay(root, cancel_event, context);
    if (overlay_out != nullptr) {
        *overlay_out = overlay;
    }
    lv_obj_t* sheet = Panel(layout, overlay);
    lv_obj_set_width(sheet, layout.width - layout.safe_horizontal * 2);
    lv_obj_set_style_border_color(sheet, lv_color_hex(border_color), 0);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -layout.safe_horizontal);
    return sheet;
}

}  // namespace

void SystemDetailUi::ResetActiveScreen() {
    power_animation_refresh_.Stop();
    root_ = nullptr;
    action_sink_ = nullptr;
    action_context_ = nullptr;
    power_switch_ = nullptr;
    power_timeout_ = nullptr;
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
        RequestDisplayRefresh(lv_obj_get_display(ui->root_));
    }
}

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowSystemInformationLocked(
    lv_obj_t* root, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    ResetActiveScreen();
    root_ = root;
    system_information_model_ = model;
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kSystemInformation;
    RenderSystemInformationLocked();
    ESP_LOGI(kTag, "System Information visible: chip=%s", model.host_chip.data());
    return {};
}

void SystemDetailUi::RenderSystemInformationLocked() {
    if (!SystemInformationVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    if (system_information_model_.firmware_update_view) {
        RenderFirmwareUpdateLocked();
        return;
    }
    Header(layout_, root_, "System Information", "Device, firmware and memory", SystemInformationBackEvent, this);
    lv_obj_t* scroll = Scroll(layout_, root_, ScrollEvent, this);

    lv_obj_t* firmware = Panel(layout_, scroll);
    (void)Label(firmware, "MICROPIXEL FIRMWARE", SystemFontRole::kSmall, 0x91a4bdU);
    (void)Label(firmware, system_information_model_.firmware_version.data(), SystemFontRole::kLarge, 0xf2f7ffU);
    char build[144]{};
    std::snprintf(build, sizeof(build), "%.40s %.24s  %.48s", system_information_model_.build_date.data(),
                  system_information_model_.build_time.data(), system_information_model_.build_id.data());
    lv_obj_t* build_label = Label(firmware, build, SystemFontRole::kSmall, 0x69a7ffU);
    lv_obj_set_width(build_label, LV_PCT(100));
    lv_label_set_long_mode(build_label, LV_LABEL_LONG_DOT);
    const char* update_status = DisplayText(system_information_model_.firmware_update_message.data(),
                                            "Connect to Control to check for updates");
    lv_obj_t* update_status_label = Label(firmware, update_status, SystemFontRole::kSmall, 0x91a4bdU);
    lv_obj_set_width(update_status_label, LV_PCT(100));
    lv_label_set_long_mode(update_status_label, LV_LABEL_LONG_DOT);
    if (system_information_model_.firmware_update_installable) {
        char update_action[96]{};
        std::snprintf(update_action, sizeof(update_action),
                      system_information_model_.firmware_update_available ? "Update to %s" : "Reinstall %s",
                      DisplayText(system_information_model_.latest_firmware_version.data(),
                                  system_information_model_.firmware_version.data()));
        lv_obj_t* update = Button(layout_, firmware, update_action, 0x69a7ffU);
        lv_obj_add_event_cb(update, SystemInformationUpdateEvent, LV_EVENT_SHORT_CLICKED, this);
    }

    SectionLabel(scroll, "HARDWARE");
    lv_obj_t* hardware = Panel(layout_, scroll, 0);
    InformationRow(layout_, hardware, "Host chip", system_information_model_.host_chip.data());
    InformationRow(layout_, hardware, "CPU", system_information_model_.cpu.data());
    InformationRow(layout_, hardware, "Wi-Fi coprocessor", system_information_model_.wifi_coprocessor.data());
    InformationRow(layout_, hardware, "Wi-Fi MAC", system_information_model_.wifi_mac.data());
    InformationRow(layout_, hardware, "Flash", system_information_model_.flash_capacity.data());

    SectionLabel(scroll, "DISPLAY");
    lv_obj_t* display = Panel(layout_, scroll, 0);
    InformationRow(layout_, display, "Panel", system_information_model_.panel.data());
    InformationRow(layout_, display, "Interface", system_information_model_.display_interface.data());
    InformationRow(layout_, display, "Resolution", system_information_model_.resolution.data());
    InformationRow(layout_, display, "Touch", system_information_model_.touch_controller.data());

    SectionLabel(scroll, "MEMORY");
    lv_obj_t* memory = Panel(layout_, scroll, 0);
    char sram_total[24]{}, sram_free[24]{}, psram_total[24]{}, psram_free[24]{};
    FormatSize(system_information_model_.internal_sram.total_kib, sram_total, sizeof(sram_total));
    FormatSize(system_information_model_.internal_sram.free_kib, sram_free, sizeof(sram_free));
    FormatSize(system_information_model_.psram.total_kib, psram_total, sizeof(psram_total));
    FormatSize(system_information_model_.psram.free_kib, psram_free, sizeof(psram_free));
    InformationRow(layout_, memory, "SRAM total", sram_total);
    InformationRow(layout_, memory, "SRAM free", sram_free);
    InformationRow(layout_, memory, "PSRAM total", psram_total);
    InformationRow(layout_, memory, "PSRAM free", psram_free);

    SectionLabel(scroll, "RUNTIME");
    lv_obj_t* runtime = Panel(layout_, scroll, 0);
    InformationRow(layout_, runtime, "ESP-IDF", system_information_model_.idf_version.data());
    InformationRow(layout_, runtime, "Uptime", system_information_model_.uptime.data());
    InformationRow(layout_, runtime, "Last reset", system_information_model_.last_reset.data());
    InformationRow(layout_, runtime, "Build date", system_information_model_.build_date.data());
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::RenderFirmwareUpdateLocked() {
    const bool in_progress = FirmwareUpdateInProgress(system_information_model_.firmware_update_state);
    lv_obj_t* header = CreateSystemHeader(root_, layout_, "Firmware Update", "Keep the device powered on",
                                          SystemInformationBackEvent, this);
    if (in_progress) {
        lv_obj_t* back = lv_obj_get_child(header, 0);
        if (back != nullptr) {
            lv_obj_add_state(back, LV_STATE_DISABLED);
            lv_obj_set_style_opa(back, LV_OPA_30, 0);
        }
    }

    lv_obj_t* content = Scroll(layout_, root_, ScrollEvent, this);
    lv_obj_t* panel = Panel(layout_, content);
    lv_obj_set_style_pad_row(panel, layout_.panel_gap, 0);
    lv_obj_t* icon =
        Label(panel, LV_SYMBOL_REFRESH, SystemFontRole::kTitle,
              system_information_model_.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? 0xff6b74U
                                                                                                       : 0x69a7ffU);
    lv_obj_set_width(icon, LV_PCT(100));
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    const char* stage = FirmwareUpdateStageText(system_information_model_.firmware_update_state);
    lv_obj_t* stage_label = Label(panel, stage, layout_.heading_font, 0xf2f7ffU);
    lv_obj_set_width(stage_label, LV_PCT(100));
    lv_obj_set_style_text_align(stage_label, LV_TEXT_ALIGN_CENTER, 0);

    char versions[160]{};
    const char* latest = DisplayText(system_information_model_.latest_firmware_version.data(),
                                     system_information_model_.firmware_version.data());
    std::snprintf(versions, sizeof(versions), "%s  %s  %s", system_information_model_.firmware_version.data(),
                  LV_SYMBOL_RIGHT, latest);
    lv_obj_t* version_label = Label(panel, versions, layout_.detail_font, 0x91a4bdU);
    lv_obj_set_width(version_label, LV_PCT(100));
    lv_obj_set_style_text_align(version_label, LV_TEXT_ALIGN_CENTER, 0);

    const uint8_t progress = std::min<uint8_t>(system_information_model_.firmware_progress_percent, 100U);
    char percent[16]{};
    std::snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(progress));
    lv_obj_t* percent_label = Label(panel, percent, SystemFontRole::kTitle, 0xf2f7ffU);
    lv_obj_set_width(percent_label, LV_PCT(100));
    lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* progress_bar = lv_bar_create(panel);
    lv_obj_set_size(progress_bar, LV_PCT(100), std::max<int32_t>(12, layout_.control_height / 4));
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x21364eU), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x69a7ffU), LV_PART_INDICATOR);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, progress, LV_ANIM_OFF);

    if (system_information_model_.firmware_size_bytes != 0U) {
        char size[64]{};
        const uint32_t shown_bytes =
            std::min(system_information_model_.firmware_processed_bytes, system_information_model_.firmware_size_bytes);
        const uint32_t shown_tenths =
            static_cast<uint32_t>((static_cast<uint64_t>(shown_bytes) * 10U) / (1024U * 1024U));
        const uint32_t total_tenths = static_cast<uint32_t>(
            (static_cast<uint64_t>(system_information_model_.firmware_size_bytes) * 10U) / (1024U * 1024U));
        std::snprintf(size, sizeof(size), "%" PRIu32 ".%" PRIu32 " / %" PRIu32 ".%" PRIu32 " MB", shown_tenths / 10U,
                      shown_tenths % 10U, total_tenths / 10U, total_tenths % 10U);
        lv_obj_t* size_label = Label(panel, size, layout_.detail_font, 0x91a4bdU);
        lv_obj_set_width(size_label, LV_PCT(100));
        lv_obj_set_style_text_align(size_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t* message =
        Label(panel, DisplayText(system_information_model_.firmware_update_message.data(), stage), layout_.detail_font,
              system_information_model_.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? 0xff8a91U
                                                                                                       : 0x91a4bdU);
    lv_obj_set_width(message, LV_PCT(100));
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);

    if (!in_progress && system_information_model_.firmware_update_installable) {
        const char* button_text =
            system_information_model_.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? "Retry update"
            : system_information_model_.firmware_update_available ? "Download and install"
                                                                  : "Reinstall current version";
        lv_obj_t* update = Button(layout_, panel, button_text, 0xc5f36dU);
        lv_obj_add_event_cb(update, SystemInformationUpdateEvent, LV_EVENT_SHORT_CLICKED, this);
    }
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::UpdateSystemInformationLocked(const host_ui::SystemInformationModel& model) {
    if (!SystemInformationVisible()) {
        return;
    }
    system_information_model_ = model;
    RenderSystemInformationLocked();
}

void SystemDetailUi::LeaveSystemInformation() {
    if (SystemInformationVisible()) {
        system_information_model_ = {};
        ResetActiveScreen();
    }
}

bool SystemDetailUi::SystemInformationVisible() const { return active_screen_ == Screen::kSystemInformation; }

void* SystemDetailUi::SystemInformationActionContext() const { return action_context_; }

void SystemDetailUi::SystemInformationBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseSystemInformation});
    }
}

void SystemDetailUi::SystemInformationUpdateEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr && ui->system_information_model_.firmware_update_installable) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kInstallFirmwareUpdate});
    }
}

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
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    Header(layout_, root_, "Power Management", "Idle and automatic sleep", PowerManagementBackEvent, this);
    lv_obj_t* content = Scroll(layout_, root_, ScrollEvent, this);

    lv_obj_t* automatic = Panel(layout_, content);
    lv_obj_set_flex_flow(automatic, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(automatic, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(automatic, 12, 0);
    lv_obj_t* automatic_text = square_common::CreateSystemColumn(automatic, 2);
    lv_obj_set_width(automatic_text, 0);
    lv_obj_set_flex_grow(automatic_text, 1);
    (void)Label(automatic_text, "Auto sleep", SystemFontRole::kLarge, 0xf2f7ffU);
    (void)Label(automatic_text, "Sleep when the device is idle", SystemFontRole::kSmall, 0x91a4bdU);
    power_switch_ = lv_switch_create(automatic);
    lv_obj_set_size(power_switch_, 64, 36);
    lv_obj_add_event_cb(power_switch_, PowerManagementSwitchEvent, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* timeout = Panel(layout_, content);
    (void)Label(timeout, "Idle timeout", SystemFontRole::kLarge, 0xf2f7ffU);
    (void)Label(timeout, "Choose how long to wait", SystemFontRole::kSmall, 0x91a4bdU);
    power_timeout_ = lv_dropdown_create(timeout);
    lv_dropdown_set_options(power_timeout_, "1 minute\n5 minutes\n10 minutes\n30 minutes");
    lv_obj_set_size(power_timeout_, LV_PCT(100), layout_.control_height);
    lv_obj_set_style_text_font(power_timeout_, BuiltinLatinFont(SystemFontRole::kMedium), 0);
    lv_obj_set_style_text_font(power_timeout_, BuiltinLatinFont(SystemFontRole::kMedium), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(power_timeout_, lv_color_hex(0x24364eU), LV_PART_INDICATOR);
    lv_obj_add_event_cb(power_timeout_, PowerManagementTimeoutEvent, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t* note = Label(content, "External power pauses the idle timer. Unplugging starts a fresh countdown.",
                           layout_.detail_font, 0x748aa5U);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    UpdatePowerManagementLocked(power_management_model_);
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
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
    RequestDisplayRefresh(lv_obj_get_display(root_));
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
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
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
    (void)Label(status_heading, RemoteState(remote_control_model_.connection_state), SystemFontRole::kLarge, 0xf2f7ffU);
    InformationRow(layout_, status, "Service",
                   remote_control_model_.service[0] != '\0' ? remote_control_model_.service.data() : "Not configured");

    lv_obj_t* pairing = Panel(layout_, scroll);
    (void)Label(pairing, "Connect this device", SystemFontRole::kLarge, 0xf2f7ffU);
    if (remote_control_model_.pairing_code_available) {
        lv_obj_t* code = Label(pairing, remote_control_model_.pairing_code.data(), SystemFontRole::kTitle, 0xc5f36dU);
        lv_obj_set_width(code, LV_PCT(100));
        lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
        char expiry[48]{};
        std::snprintf(expiry, sizeof(expiry), "Expires in %02" PRIu32 ":%02" PRIu32,
                      remote_control_model_.pairing_expires_seconds / 60U,
                      remote_control_model_.pairing_expires_seconds % 60U);
        lv_obj_t* expiry_label = Label(pairing, expiry, SystemFontRole::kSmall, 0x91a4bdU);
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
        lv_obj_t* generate = Button(layout_, pairing, text, available ? 0xc5f36dU : 0x91a4bdU);
        if (available) {
            lv_obj_add_event_cb(generate, RemoteControlPairingEvent, LV_EVENT_SHORT_CLICKED, this);
        } else {
            lv_obj_remove_flag(generate, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    lv_obj_t* note =
        Label(pairing, "Codes are single-use and expire after 5 minutes", SystemFontRole::kSmall, 0x748aa5U);
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* toggle =
        Button(layout_, scroll, remote_control_model_.enabled ? "Turn Remote Control Off" : "Turn Remote Control On",
               remote_control_model_.enabled ? 0xff6b74U : 0x69a7ffU);
    lv_obj_add_event_cb(toggle, RemoteControlToggleEvent, LV_EVENT_SHORT_CLICKED, this);
    if (remote_control_off_confirmation_visible_) {
        DrawRemoteControlOffConfirmationLocked();
    }
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
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
    lv_obj_t* sheet = CreateActionSheet(layout_, root_, RemoteControlConfirmationCancelEvent, this, 0x653c48U);
    (void)Label(sheet, "Turn Off Remote Control?", SystemFontRole::kLarge, 0xf2f7ffU);
    lv_obj_t* detail =
        Label(sheet, "Remote access and active connection codes will stop.", SystemFontRole::kMedium, 0x91a4bdU);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* turn_off = Button(layout_, sheet, "Turn Off", 0xff6b74U);
    lv_obj_add_event_cb(turn_off, RemoteControlConfirmOffEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel");
    lv_obj_add_event_cb(cancel, RemoteControlConfirmationCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowAppManagementLocked(
    lv_obj_t* root, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    ResetActiveScreen();
    root_ = root;
    app_management_model_ = model;
    app_management_selected_index_ = 0U;
    app_management_overlay_ = AppOverlay::kNone;
    action_sink_ = action_sink;
    action_context_ = action_context;
    active_screen_ = Screen::kAppManagement;
    lv_display_t* display = lv_obj_get_display(root_);
    if (display != nullptr && app_management_probe_display_ == nullptr) {
        app_management_probe_display_ = display;
        lv_display_add_event_cb(display, AppManagementDisplayEvent, LV_EVENT_RENDER_READY, this);
        lv_display_add_event_cb(display, AppManagementDisplayEvent, LV_EVENT_REFR_READY, this);
    }
    RenderAppManagementLocked();
    return {};
}

void SystemDetailUi::RenderAppManagementLocked() {
    if (!AppManagementVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    Header(layout_, root_, "App Management", "Installed apps and storage", AppManagementBackEvent, this);
    lv_obj_t* scroll = Scroll(layout_, root_, ScrollEvent, this);
    char storage[80]{};
    const uint32_t used_tenths =
        static_cast<uint32_t>((static_cast<uint64_t>(app_management_model_.storage_used_kib) * 10U) / 1024U);
    const uint32_t total_tenths =
        static_cast<uint32_t>((static_cast<uint64_t>(app_management_model_.storage_total_kib) * 10U) / 1024U);
    std::snprintf(storage, sizeof(storage), "Storage %" PRIu32 ".%" PRIu32 " / %" PRIu32 ".%" PRIu32 " MB",
                  used_tenths / 10U, used_tenths % 10U, total_tenths / 10U, total_tenths % 10U);
    (void)Label(scroll, storage, SystemFontRole::kSmall, 0x91a4bdU);
    app_bindings_ = {};

    if (app_management_model_.app_count == 0U) {
        lv_obj_t* empty = Panel(layout_, scroll);
        (void)Label(empty, "No apps installed", SystemFontRole::kLarge, 0xf2f7ffU);
    }
    for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
        const host_ui::InstalledAppModel& app = app_management_model_.apps[index];
        app_bindings_[index] = {.ui = this, .index = index};
        lv_obj_t* row = CreateSystemButtonPanel(scroll, layout_);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_add_event_cb(row, AppManagementRowEvent, LV_EVENT_SHORT_CLICKED, &app_bindings_[index]);
        lv_obj_t* app_text = square_common::CreateSystemColumn(row, 4);
        lv_obj_set_width(app_text, 0);
        lv_obj_set_flex_grow(app_text, 1);
        lv_obj_t* name = Label(app_text, app.display_name, SystemFontRole::kLarge, 0xf2f7ffU);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        char size[28]{};
        FormatSize(app.bundle_size_kib, size, sizeof(size));
        (void)Label(app_text, size, SystemFontRole::kSmall, 0x91a4bdU);

        (void)square_common::CreateSystemMoreIndicator(row, 32, 52, 6, 5);
    }
    RenderAppManagementOverlayLocked();
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::RenderAppManagementOverlayLocked() {
    if (!AppManagementVisible() || root_ == nullptr) {
        return;
    }
    if (app_management_overlay_root_ != nullptr) {
        lv_obj_delete(app_management_overlay_root_);
        app_management_overlay_root_ = nullptr;
    }
    switch (app_management_overlay_) {
        case AppOverlay::kActions:
            DrawAppManagementActionsLocked();
            break;
        case AppOverlay::kInformation:
            DrawAppManagementInformationLocked();
            break;
        case AppOverlay::kUninstallConfirmation:
            DrawAppManagementUninstallConfirmationLocked();
            break;
        case AppOverlay::kUninstallUnavailable:
            DrawAppManagementUninstallUnavailableLocked();
            break;
        case AppOverlay::kNone:
        default:
            break;
    }
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::LeaveAppManagement() {
    if (AppManagementVisible()) {
        app_management_model_ = {};
        app_bindings_ = {};
        ResetActiveScreen();
    }
}

bool SystemDetailUi::AppManagementVisible() const { return active_screen_ == Screen::kAppManagement; }

void* SystemDetailUi::AppManagementActionContext() const { return action_context_; }

void SystemDetailUi::AppManagementBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseAppManagement});
    }
}

void SystemDetailUi::AppManagementRenderAsync(void* context) {
    auto* ui = static_cast<SystemDetailUi*>(context);
    if (ui != nullptr && ui->AppManagementVisible()) {
        ui->StartAppManagementLatencyProbe();
        ui->RenderAppManagementOverlayLocked();
    }
}

void SystemDetailUi::BeginAppManagementLatencyProbe(const char* operation) {
    app_management_probe_operation_ = operation;
    app_management_probe_touch_us_ = esp_timer_get_time();
    app_management_probe_render_start_us_ = 0;
    app_management_probe_render_ready_us_ = 0;
    app_management_probe_armed_ = false;
}

void SystemDetailUi::StartAppManagementLatencyProbe() {
    if (app_management_probe_touch_us_ == 0) {
        return;
    }
    app_management_probe_render_start_us_ = esp_timer_get_time();
    app_management_probe_armed_ = true;
}

void SystemDetailUi::AppManagementDisplayEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || !ui->app_management_probe_armed_) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (lv_event_get_code(event) == LV_EVENT_RENDER_READY) {
        if (ui->app_management_probe_render_ready_us_ == 0) {
            ui->app_management_probe_render_ready_us_ = now;
        }
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_REFR_READY || ui->app_management_probe_render_ready_us_ == 0) {
        return;
    }
    const int64_t queue_us = ui->app_management_probe_render_start_us_ - ui->app_management_probe_touch_us_;
    const int64_t render_us = ui->app_management_probe_render_ready_us_ - ui->app_management_probe_render_start_us_;
    const int64_t refresh_us = now - ui->app_management_probe_render_ready_us_;
    const int64_t total_us = now - ui->app_management_probe_touch_us_;
    ESP_LOGI(kTag,
             "latency %s: queue=%" PRId64 ".%03" PRId64 " render=%" PRId64 ".%03" PRId64 " flush=%" PRId64 ".%03" PRId64
             " total=%" PRId64 ".%03" PRId64 " ms",
             ui->app_management_probe_operation_ != nullptr ? ui->app_management_probe_operation_ : "unknown",
             queue_us / 1000, queue_us % 1000, render_us / 1000, render_us % 1000, refresh_us / 1000, refresh_us % 1000,
             total_us / 1000, total_us % 1000);
    ui->app_management_probe_touch_us_ = 0;
    ui->app_management_probe_armed_ = false;
}

void SystemDetailUi::QueueAppManagementRender() {
    if (lv_async_call(AppManagementRenderAsync, this) != LV_RESULT_OK) {
        ESP_LOGW(kTag, "failed to queue App Management render");
    }
}

void SystemDetailUi::AppManagementRowEvent(lv_event_t* event) {
    auto* binding = static_cast<AppBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->ui == nullptr ||
        binding->index >= binding->ui->app_management_model_.app_count) {
        return;
    }
    binding->ui->app_management_selected_index_ = binding->index;
    binding->ui->app_management_overlay_ = AppOverlay::kActions;
    binding->ui->BeginAppManagementLatencyProbe("actions.open");
    binding->ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = AppOverlay::kNone;
        ui->BeginAppManagementLatencyProbe("sheet.close");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementOpenEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr &&
        ui->app_management_selected_index_ < ui->app_management_model_.app_count &&
        ui->app_management_model_.launch_available) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchInstalledApp,
                                                 .app_index = ui->app_management_selected_index_});
    }
}

void SystemDetailUi::AppManagementInformationEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = AppOverlay::kInformation;
        ui->BeginAppManagementLatencyProbe("information.open");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->app_management_overlay_ = ui->app_management_model_.uninstall_available ? AppOverlay::kUninstallConfirmation
                                                                                    : AppOverlay::kUninstallUnavailable;
        ui->BeginAppManagementLatencyProbe("uninstall.open");
        ui->QueueAppManagementRender();
    }
}

void SystemDetailUi::AppManagementConfirmUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr && ui->app_management_model_.uninstall_available &&
        ui->app_management_selected_index_ < ui->app_management_model_.app_count) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kUninstallInstalledApp,
                                                 .app_index = ui->app_management_selected_index_});
    }
}

void SystemDetailUi::DrawAppManagementActionsLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet =
        CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, 0x42607fU, &app_management_overlay_root_);
    lv_obj_t* title = Label(sheet, app.display_name, SystemFontRole::kLarge, 0xf2f7ffU);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* open = Button(layout_, sheet, app_management_model_.launch_available ? "Open" : "Open unavailable",
                            app_management_model_.launch_available ? 0xf2f7ffU : 0x708198U);
    if (app_management_model_.launch_available) {
        lv_obj_add_event_cb(open, AppManagementOpenEvent, LV_EVENT_SHORT_CLICKED, this);
    } else {
        lv_obj_remove_flag(open, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(open, LV_OPA_40, 0);
    }
    lv_obj_t* information = Button(layout_, sheet, "App Information");
    lv_obj_add_event_cb(information, AppManagementInformationEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* uninstall = Button(layout_, sheet, "Uninstall", 0xff6b74U);
    lv_obj_add_event_cb(uninstall, AppManagementUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel", 0x91a4bdU);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementInformationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet =
        CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, 0x42607fU, &app_management_overlay_root_);
    lv_obj_t* title = Label(sheet, app.display_name, SystemFontRole::kLarge, 0xf2f7ffU);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* details = Panel(layout_, sheet, 0);
    InformationRow(layout_, details, "App ID", app.app_id != nullptr ? app.app_id : "Unknown");
    char size[24]{};
    FormatSize(app.bundle_size_kib, size, sizeof(size));
    InformationRow(layout_, details, "Bundle Size", size);
    InformationRow(layout_, details, "Runtime", "WebAssembly AOT");
    lv_obj_t* done = Button(layout_, sheet, "Done");
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallUnavailableLocked() {
    lv_obj_t* sheet =
        CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, 0x42607fU, &app_management_overlay_root_);
    (void)Label(sheet, "Uninstall unavailable", SystemFontRole::kLarge, 0xf2f7ffU);
    lv_obj_t* detail = Label(sheet, "Close the running App from the Hall before uninstalling Apps.",
                             SystemFontRole::kMedium, 0x91a4bdU);
    lv_obj_set_width(detail, LV_PCT(100));
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* done = Button(layout_, sheet, "Done");
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallConfirmationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    lv_obj_t* sheet =
        CreateActionSheet(layout_, root_, AppManagementCancelEvent, this, 0x653c48U, &app_management_overlay_root_);
    (void)Label(sheet, "Uninstall App?", SystemFontRole::kLarge, 0xf2f7ffU);
    lv_obj_t* name = Label(sheet, app.display_name, SystemFontRole::kMedium, 0x91a4bdU);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t* uninstall = Button(layout_, sheet, "Uninstall", 0xff6b74U);
    lv_obj_add_event_cb(uninstall, AppManagementConfirmUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = Button(layout_, sheet, "Cancel");
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

}  // namespace micropixel::platform::lvgl::square_common
