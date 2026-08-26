#include "platform/metalio-claw4/system_menu_ui.hpp"

#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "platform/metalio-claw4/lvgl_wakeup.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "system_menu_ui";
constexpr int32_t kScreenWidth = 720;
constexpr int32_t kScreenHeight = 720;
constexpr int32_t kHeaderHeight = 108;
constexpr int32_t kRowX = 40;
constexpr int32_t kRowTop = 8;
constexpr int32_t kRowWidth = 640;
constexpr int32_t kRowHeight = 96;
constexpr int32_t kRowStride = 108;
constexpr size_t kRowCount = 6U;
constexpr int32_t kContentHeight = kRowTop + static_cast<int32_t>(kRowCount) * kRowStride + 8;

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int32_t x, int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

const char* WifiDetail(const host_ui::SystemMenuModel& model) {
    return !model.wifi_available   ? "Not available"
           : model.wifi_connected  ? "Connected"
           : model.wifi_connecting ? "Connecting..."
           : model.wifi_enabled    ? "Not connected"
                                   : "Off";
}

const char* RemoteControlDetail(const host_ui::SystemMenuModel& model) {
    return model.remote_control_connected ? "Connected" : model.remote_control_enabled ? "Not connected" : "Off";
}

const char* PowerManagementDetail(const host_ui::SystemMenuModel& model) {
    switch (model.auto_sleep_timeout_minutes) {
        case 0U:
            return "Auto sleep off";
        case 1U:
            return "Auto sleep after 1 minute";
        case 5U:
            return "Auto sleep after 5 minutes";
        case 10U:
            return "Auto sleep after 10 minutes";
        case 30U:
            return "Auto sleep after 30 minutes";
        default:
            return "Auto sleep configured";
    }
}

std::array<char, host_ui::kFirmwareUpdateMessageCapacity> FirmwareDetail(const host_ui::SystemMenuModel& model) {
    std::array<char, host_ui::kFirmwareUpdateMessageCapacity> detail{};
    if (model.firmware_update_state == host_ui::FirmwareUpdateState::kDownloading) {
        std::snprintf(detail.data(), detail.size(), "Downloading update...");
    } else if (model.firmware_update_state == host_ui::FirmwareUpdateState::kVerifying) {
        std::snprintf(detail.data(), detail.size(), "Verifying update...");
    } else if (model.firmware_update_state == host_ui::FirmwareUpdateState::kInstalling) {
        std::snprintf(detail.data(), detail.size(), "Installing update...");
    } else if (model.firmware_update_available) {
        std::snprintf(detail.data(), detail.size(), "Update %s available", model.latest_firmware_version.data());
    } else if (model.firmware_update_state == host_ui::FirmwareUpdateState::kCurrent) {
        std::snprintf(detail.data(), detail.size(), "Up to date");
    } else {
        std::snprintf(detail.data(), detail.size(), "Device and software");
    }
    return detail;
}

}  // namespace

void SystemMenuUi::ResetObjectPointers() {
    scroll_content_ = nullptr;
    wifi_detail_label_ = nullptr;
    remote_control_detail_label_ = nullptr;
    system_information_detail_label_ = nullptr;
    power_management_detail_label_ = nullptr;
    firmware_update_dot_ = nullptr;
    row_bindings_ = {};
}

void SystemMenuUi::DrawRow(lv_obj_t* parent, size_t index, host_ui::SystemMenuItem item, const char* icon_text,
                           const char* name, const char* detail, uint32_t icon_color, bool emphasized_border) {
    if (parent == nullptr || index >= row_bindings_.size()) {
        return;
    }
    row_bindings_[index] = RowBinding{.ui = this, .item = item};

    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, kRowX, kRowTop + static_cast<int32_t>(index) * kRowStride);
    lv_obj_set_size(panel, kRowWidth, kRowHeight);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 22, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(emphasized_border ? 0x42607fU : 0x2e4562U), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x111f32U), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, RowEvent, LV_EVENT_SHORT_CLICKED, &row_bindings_[index]);

    lv_obj_t* icon = lv_label_create(panel);
    lv_label_set_text(icon, icon_text);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(icon_color), 0);
    lv_obj_set_size(icon, 56, 38);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(icon, 26, 28);
    (void)CreateLabel(panel, name, &lv_font_montserrat_24, 0xf2f7ffU, 104, 17);
    lv_obj_t* detail_label = CreateLabel(panel, detail, &lv_font_montserrat_18, 0x91a4bdU, 104, 54);
    switch (item) {
        case host_ui::SystemMenuItem::kWifi:
            wifi_detail_label_ = detail_label;
            break;
        case host_ui::SystemMenuItem::kRemoteControl:
            remote_control_detail_label_ = detail_label;
            break;
        case host_ui::SystemMenuItem::kSystemInformation:
            system_information_detail_label_ = detail_label;
            firmware_update_dot_ = lv_obj_create(panel);
            lv_obj_set_pos(firmware_update_dot_, kRowWidth - 73, 22);
            lv_obj_set_size(firmware_update_dot_, 10, 10);
            lv_obj_set_style_pad_all(firmware_update_dot_, 0, 0);
            lv_obj_set_style_radius(firmware_update_dot_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(firmware_update_dot_, 0, 0);
            lv_obj_set_style_bg_color(firmware_update_dot_, lv_color_hex(0xef5d4fU), 0);
            lv_obj_remove_flag(firmware_update_dot_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(firmware_update_dot_, LV_OBJ_FLAG_CLICKABLE);
            break;
        case host_ui::SystemMenuItem::kPowerManagement:
            power_management_detail_label_ = detail_label;
            break;
        case host_ui::SystemMenuItem::kLanguage:
        case host_ui::SystemMenuItem::kManageApps:
            break;
    }
    lv_obj_t* chevron = lv_label_create(panel);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(chevron, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_set_pos(chevron, kRowWidth - 46, 34);
}

void SystemMenuUi::Update(const host_ui::SystemMenuModel& model) {
    if (wifi_detail_label_ == nullptr || remote_control_detail_label_ == nullptr ||
        system_information_detail_label_ == nullptr || power_management_detail_label_ == nullptr ||
        firmware_update_dot_ == nullptr || display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    lv_label_set_text(wifi_detail_label_, WifiDetail(model));
    lv_label_set_text(remote_control_detail_label_, RemoteControlDetail(model));
    lv_label_set_text(power_management_detail_label_, PowerManagementDetail(model));
    const auto firmware_detail = FirmwareDetail(model);
    lv_label_set_text(system_information_detail_label_, firmware_detail.data());
    if (model.firmware_update_available) {
        lv_obj_remove_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
    }
    RequestDisplayRefresh(display_);
    esp_lv_adapter_unlock();
}

void SystemMenuUi::BackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemMenuUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseSystemMenu});
    }
}

void SystemMenuUi::RowEvent(lv_event_t* event) {
    auto* binding = static_cast<RowBinding*>(lv_event_get_user_data(event));
    if (binding != nullptr && binding->ui != nullptr && binding->ui->action_sink_ != nullptr) {
        binding->ui->action_sink_(binding->ui->action_context_,
                                  host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSelectSystemMenuItem,
                                                          .value = static_cast<uint32_t>(binding->item)});
    }
}

void SystemMenuUi::ScrollEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemMenuUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        RequestDisplayRefresh(ui->display_);
    }
}

std::expected<void, host_ui::SystemUiError> SystemMenuUi::ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                     const host_ui::SystemMenuModel& model,
                                                                     host_ui::SystemUiActionSink action_sink,
                                                                     void* action_context) {
    if (root == nullptr || display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    display_ = display;
    action_sink_ = action_sink;
    action_context_ = action_context;
    ResetObjectPointers();

    lv_obj_t* back = lv_obj_create(root);
    lv_obj_set_pos(back, 40, 32);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0d1929U), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, BackEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_center(back_label);

    (void)CreateLabel(root, "System Settings", &lv_font_montserrat_32, 0xf2f7ffU, 116, 28);
    (void)CreateLabel(root, "Configure this device", &lv_font_montserrat_18, 0x91a4bdU, 116, 70);

    scroll_content_ = lv_obj_create(root);
    lv_obj_set_pos(scroll_content_, 0, kHeaderHeight);
    lv_obj_set_size(scroll_content_, kScreenWidth, kScreenHeight - kHeaderHeight);
    lv_obj_set_style_pad_all(scroll_content_, 0, 0);
    lv_obj_set_style_border_width(scroll_content_, 0, 0);
    lv_obj_set_style_bg_opa(scroll_content_, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll_content_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_content_, LV_SCROLLBAR_MODE_AUTO);
    constexpr lv_obj_flag_t kScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(scroll_content_, kScrollFlags);
    lv_obj_add_event_cb(scroll_content_, ScrollEvent, LV_EVENT_SCROLL, this);
    lv_obj_add_event_cb(scroll_content_, ScrollEvent, LV_EVENT_SCROLL_END, this);
    lv_obj_set_style_width(scroll_content_, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll_content_, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll_content_, lv_color_hex(0x42607fU), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll_content_, LV_OPA_COVER, LV_PART_SCROLLBAR);

    DrawRow(scroll_content_, 0U, host_ui::SystemMenuItem::kWifi, LV_SYMBOL_WIFI, "Wi-Fi", WifiDetail(model), 0x69a7ffU,
            true);
    DrawRow(scroll_content_, 1U, host_ui::SystemMenuItem::kRemoteControl, LV_SYMBOL_REFRESH, "Remote Control",
            RemoteControlDetail(model), 0xc5f36dU, true);
    const auto firmware_detail = FirmwareDetail(model);
    DrawRow(scroll_content_, 2U, host_ui::SystemMenuItem::kSystemInformation, "i", "System Information",
            firmware_detail.data(), 0x69a7ffU);
    if (firmware_update_dot_ != nullptr && !model.firmware_update_available) {
        lv_obj_add_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
    }
    DrawRow(scroll_content_, 3U, host_ui::SystemMenuItem::kLanguage, "A", "Language",
            model.language != nullptr ? model.language : "English", 0x4dd6a4U);
    DrawRow(scroll_content_, 4U, host_ui::SystemMenuItem::kPowerManagement, LV_SYMBOL_POWER, "Power Management",
            PowerManagementDetail(model), 0xf4c86aU);
    DrawRow(scroll_content_, 5U, host_ui::SystemMenuItem::kManageApps, LV_SYMBOL_LIST, "Manage Apps",
            model.installed_app_count == 1U ? "1 installed app" : "Installed apps", 0xff9f43U);

    lv_obj_t* bottom_spacer = lv_obj_create(scroll_content_);
    lv_obj_set_pos(bottom_spacer, 0, kContentHeight - 1);
    lv_obj_set_size(bottom_spacer, 1, 1);
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_move_foreground(root);
    RequestDisplayRefresh(display_);
    ESP_LOGI(kTag, "System Settings visible: wifi=%s apps=%" PRIu32,
             model.wifi_available ? (model.wifi_connected ? "connected" : "available") : "unavailable",
             model.installed_app_count);
    return {};
}

void SystemMenuUi::Deactivate() {
    action_sink_ = nullptr;
    action_context_ = nullptr;
    ResetObjectPointers();
    display_ = nullptr;
}

}  // namespace micropixel::platform::metalio_claw4
