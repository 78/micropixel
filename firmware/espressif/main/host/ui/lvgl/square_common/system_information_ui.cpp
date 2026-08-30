#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "host/ui/lvgl/square_common/system_detail_ui.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui_internal.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
using system_detail_internal::Button;
using system_detail_internal::DisplayText;
using system_detail_internal::FirmwareUpdateInProgress;
using system_detail_internal::FirmwareUpdateStageText;
using system_detail_internal::FormatSize;
using system_detail_internal::Header;
using system_detail_internal::InformationRow;
using system_detail_internal::kTag;
using system_detail_internal::Label;
using system_detail_internal::Panel;
using system_detail_internal::Scroll;
using system_detail_internal::SectionLabel;
}  // namespace

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
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    if (system_information_model_.firmware_update_view) {
        RenderFirmwareUpdateLocked();
        return;
    }
    Header(layout_, root_, "System Information", "Device, firmware and memory", SystemInformationBackEvent, this);
    lv_obj_t* scroll = Scroll(layout_, root_, ScrollEvent, this);

    lv_obj_t* firmware = Panel(layout_, scroll);
    (void)Label(firmware, "MICROPIXEL FIRMWARE", platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    (void)Label(firmware, system_information_model_.firmware_version.data(), platform::lvgl::SystemFontRole::kLarge,
                theme::kPrimaryText);
    char build[144]{};
    std::snprintf(build, sizeof(build), "%.40s %.24s  %.48s", system_information_model_.build_date.data(),
                  system_information_model_.build_time.data(), system_information_model_.build_id.data());
    lv_obj_t* build_label = Label(firmware, build, platform::lvgl::SystemFontRole::kSmall, theme::kAccent);
    lv_obj_set_width(build_label, LV_PCT(100));
    lv_label_set_long_mode(build_label, LV_LABEL_LONG_DOT);
    const char* update_status = DisplayText(system_information_model_.firmware_update_message.data(),
                                            "Connect to Control to check for updates");
    lv_obj_t* update_status_label =
        Label(firmware, update_status, platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
    lv_obj_set_width(update_status_label, LV_PCT(100));
    lv_label_set_long_mode(update_status_label, LV_LABEL_LONG_DOT);
    if (system_information_model_.firmware_update_installable) {
        char update_action[96]{};
        std::snprintf(update_action, sizeof(update_action),
                      system_information_model_.firmware_update_available ? "Update to %s" : "Reinstall %s",
                      DisplayText(system_information_model_.latest_firmware_version.data(),
                                  system_information_model_.firmware_version.data()));
        lv_obj_t* update = Button(layout_, firmware, update_action, theme::kAccent);
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
    InformationRow(layout_, display, "2D acceleration", system_information_model_.graphics_acceleration.data());
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
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
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
    lv_obj_t* icon = Label(panel, LV_SYMBOL_REFRESH, platform::lvgl::SystemFontRole::kTitle,
                           system_information_model_.firmware_update_state == host_ui::FirmwareUpdateState::kFailed
                               ? theme::kDanger
                               : theme::kAccent);
    lv_obj_set_width(icon, LV_PCT(100));
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    const char* stage = FirmwareUpdateStageText(system_information_model_.firmware_update_state);
    lv_obj_t* stage_label = Label(panel, stage, layout_.heading_font, theme::kPrimaryText);
    lv_obj_set_width(stage_label, LV_PCT(100));
    lv_obj_set_style_text_align(stage_label, LV_TEXT_ALIGN_CENTER, 0);

    char versions[160]{};
    const char* latest = DisplayText(system_information_model_.latest_firmware_version.data(),
                                     system_information_model_.firmware_version.data());
    std::snprintf(versions, sizeof(versions), "%s  %s  %s", system_information_model_.firmware_version.data(),
                  LV_SYMBOL_RIGHT, latest);
    lv_obj_t* version_label = Label(panel, versions, layout_.detail_font, theme::kSecondaryText);
    lv_obj_set_width(version_label, LV_PCT(100));
    lv_obj_set_style_text_align(version_label, LV_TEXT_ALIGN_CENTER, 0);

    const uint8_t progress = std::min<uint8_t>(system_information_model_.firmware_progress_percent, 100U);
    char percent[16]{};
    std::snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(progress));
    lv_obj_t* percent_label = Label(panel, percent, platform::lvgl::SystemFontRole::kTitle, theme::kPrimaryText);
    lv_obj_set_width(percent_label, LV_PCT(100));
    lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* progress_bar = lv_bar_create(panel);
    lv_obj_set_size(progress_bar, LV_PCT(100), std::max<int32_t>(12, layout_.control_height / 4));
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(theme::kDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(theme::kControlAccent), LV_PART_INDICATOR);
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
        lv_obj_t* size_label = Label(panel, size, layout_.detail_font, theme::kSecondaryText);
        lv_obj_set_width(size_label, LV_PCT(100));
        lv_obj_set_style_text_align(size_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t* message =
        Label(panel, DisplayText(system_information_model_.firmware_update_message.data(), stage), layout_.detail_font,
              system_information_model_.firmware_update_state == host_ui::FirmwareUpdateState::kFailed
                  ? theme::kDangerSoft
                  : theme::kSecondaryText);
    lv_obj_set_width(message, LV_PCT(100));
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);

    if (!in_progress && system_information_model_.firmware_update_installable) {
        const char* button_text =
            system_information_model_.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? "Retry update"
            : system_information_model_.firmware_update_available ? "Download and install"
                                                                  : "Reinstall current version";
        lv_obj_t* update = Button(layout_, panel, button_text, theme::kPositive);
        lv_obj_add_event_cb(update, SystemInformationUpdateEvent, LV_EVENT_SHORT_CLICKED, this);
    }
    lv_obj_move_foreground(root_);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(root_));
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

}  // namespace micropixel::host_ui::lvgl::square_common
