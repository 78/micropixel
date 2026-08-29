#include "host/ui/lvgl/square_common/system_menu_ui.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "host_strings.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

constexpr char kTag[] = "system_menu_ui";
constexpr size_t kRowCount = 7U;

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void StyleTransparentContainer(lv_obj_t* object) {
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
}

const char* WifiDetail(const host_ui::SystemMenuModel& model, const host_strings::Catalog& strings) {
    return !model.wifi_available   ? strings.Get(host_strings::Id::kCommonNotAvailable)
           : model.wifi_connected  ? strings.Get(host_strings::Id::kCommonConnected)
           : model.wifi_connecting ? strings.Get(host_strings::Id::kCommonConnecting)
           : model.wifi_enabled    ? strings.Get(host_strings::Id::kCommonNotConnected)
                                   : strings.Get(host_strings::Id::kCommonOff);
}

const char* RemoteControlDetail(const host_ui::SystemMenuModel& model, const host_strings::Catalog& strings) {
    return model.remote_control_connected ? strings.Get(host_strings::Id::kCommonConnected)
           : model.remote_control_enabled ? strings.Get(host_strings::Id::kCommonNotConnected)
                                          : strings.Get(host_strings::Id::kCommonOff);
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

const char* ThemeDetail(host_ui::SystemThemeMode mode) {
    switch (mode) {
        case host_ui::SystemThemeMode::kPureBlack:
            return "Pure Black";
        case host_ui::SystemThemeMode::kDeepBlue:
            return "Deep Blue";
        case host_ui::SystemThemeMode::kSoftIvory:
            return "Soft Ivory";
    }
    return "Pure Black";
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
    appearance_detail_label_ = nullptr;
    power_management_detail_label_ = nullptr;
    firmware_update_dot_ = nullptr;
    row_bindings_ = {};
}

void SystemMenuUi::DrawRow(lv_obj_t* parent, size_t index, host_ui::SystemMenuItem item, const char* icon_text,
                           const char* name, const char* detail, uint32_t icon_color, bool emphasized_border) {
    if (parent == nullptr || layout_ == nullptr || index >= row_bindings_.size()) {
        return;
    }
    const SystemMenuLayout& layout = *layout_;
    row_bindings_[index] = RowBinding{.ui = this, .item = item};

    lv_obj_t* panel = lv_button_create(parent);
    lv_obj_set_size(panel, LV_PCT(100), layout.row_height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, layout.row_radius, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(emphasized_border ? theme::kStrongBorder : theme::kBorder), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(theme::kPanelBackground), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(theme::kPressedBackground),
                              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, RowEvent, LV_EVENT_SHORT_CLICKED, &row_bindings_[index]);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(panel, layout.row_padding_horizontal, 0);
    lv_obj_set_style_pad_right(panel, layout.row_padding_horizontal, 0);
    lv_obj_set_style_pad_column(panel, layout.row_content_gap, 0);

    lv_obj_t* icon = CreateLabel(panel, icon_text, platform::lvgl::BuiltinLatinFont(layout.row_icon_font), icon_color);
    lv_obj_set_width(icon, layout.row_icon_width);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* text_column = lv_obj_create(panel);
    StyleTransparentContainer(text_column);
    lv_obj_set_height(text_column, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_column, 1);
    lv_obj_set_flex_flow(text_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text_column, layout.row_text_gap, 0);
    lv_obj_t* name_label =
        CreateLabel(text_column, name, platform::lvgl::BuiltinLatinFont(layout.row_name_font), theme::kPrimaryText);
    lv_obj_set_width(name_label, LV_PCT(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
    lv_obj_t* detail_label = CreateLabel(text_column, detail, platform::lvgl::BuiltinLatinFont(layout.row_detail_font),
                                         theme::kSecondaryText);
    lv_obj_set_width(detail_label, LV_PCT(100));
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
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
            lv_obj_set_size(firmware_update_dot_, layout.firmware_dot_size, layout.firmware_dot_size);
            lv_obj_set_style_pad_all(firmware_update_dot_, 0, 0);
            lv_obj_set_style_radius(firmware_update_dot_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(firmware_update_dot_, 0, 0);
            lv_obj_set_style_bg_color(firmware_update_dot_, lv_color_hex(theme::kNotification), 0);
            lv_obj_remove_flag(firmware_update_dot_, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(firmware_update_dot_, LV_OBJ_FLAG_CLICKABLE);
            break;
        case host_ui::SystemMenuItem::kPowerManagement:
            power_management_detail_label_ = detail_label;
            break;
        case host_ui::SystemMenuItem::kAppearance:
            appearance_detail_label_ = detail_label;
            break;
        case host_ui::SystemMenuItem::kLanguage:
        case host_ui::SystemMenuItem::kManageApps:
            break;
    }
    lv_obj_t* chevron = CreateLabel(panel, LV_SYMBOL_RIGHT, platform::lvgl::BuiltinLatinFont(layout.row_name_font),
                                    theme::kPrimaryText);
    lv_obj_set_width(chevron, layout.row_chevron_width);
    lv_obj_set_style_text_align(chevron, LV_TEXT_ALIGN_RIGHT, 0);
}

void SystemMenuUi::Update(const host_ui::SystemMenuModel& model) {
    if (wifi_detail_label_ == nullptr || remote_control_detail_label_ == nullptr ||
        system_information_detail_label_ == nullptr || appearance_detail_label_ == nullptr ||
        power_management_detail_label_ == nullptr || firmware_update_dot_ == nullptr || display_ == nullptr ||
        esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    const host_strings::Catalog strings = host_strings::ForTag(model.locale);
    lv_label_set_text(wifi_detail_label_, WifiDetail(model, strings));
    lv_label_set_text(remote_control_detail_label_, RemoteControlDetail(model, strings));
    lv_label_set_text(power_management_detail_label_, PowerManagementDetail(model));
    lv_label_set_text(appearance_detail_label_, ThemeDetail(model.theme_mode));
    const auto firmware_detail = FirmwareDetail(model);
    lv_label_set_text(system_information_detail_label_, firmware_detail.data());
    if (model.firmware_update_available) {
        lv_obj_remove_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
    }
    platform::lvgl::RequestDisplayRefresh(display_);
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
        platform::lvgl::RequestDisplayRefresh(ui->display_);
    }
}

std::expected<void, host_ui::SystemUiError> SystemMenuUi::ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                     const SystemMenuLayout& layout,
                                                                     const host_ui::SystemMenuModel& model,
                                                                     host_ui::SystemUiActionSink action_sink,
                                                                     void* action_context) {
    if (root == nullptr || display == nullptr || layout.width <= 0 || layout.height <= layout.header_height ||
        layout.row_height <= 0 || layout.row_gap < 0 || layout.content_padding_horizontal < 0) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    layout_ = &layout;
    display_ = display;
    action_sink_ = action_sink;
    action_context_ = action_context;
    ResetObjectPointers();
    const host_strings::Catalog strings = host_strings::ForTag(model.locale);

    lv_obj_t* header = lv_obj_create(root);
    StyleTransparentContainer(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, layout.width, layout.header_height);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(header, layout.header_padding_horizontal, 0);
    lv_obj_set_style_pad_right(header, layout.header_padding_horizontal, 0);
    lv_obj_set_style_pad_top(header, layout.header_padding_top, 0);
    lv_obj_set_style_pad_column(header, layout.header_gap, 0);

    lv_obj_t* back = lv_button_create(header);
    lv_obj_set_size(back, layout.back_button_size, layout.back_button_size);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_radius(back, layout.back_button_radius, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(theme::kStrongBorder), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(theme::kNavigationBackground), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_90, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(theme::kPressedBackground),
                              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(back, std::max<int32_t>(0, layout.back_button_hit_padding));
    lv_obj_add_event_cb(back, BackEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(theme::kPrimaryText), 0);
    lv_obj_center(back_label);

    lv_obj_t* header_text = lv_obj_create(header);
    StyleTransparentContainer(header_text);
    lv_obj_set_height(header_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(header_text, 1);
    lv_obj_set_flex_flow(header_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header_text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(header_text, layout.header_text_gap, 0);
    lv_obj_t* title =
        CreateLabel(header_text, strings.Get(host_strings::Id::kSystemSettingsTitle),
                    platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kTitle), theme::kPrimaryText);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* subtitle =
        CreateLabel(header_text, strings.Get(host_strings::Id::kSystemSettingsSubtitle),
                    platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kMedium), theme::kSecondaryText);
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    scroll_content_ = lv_obj_create(root);
    lv_obj_set_pos(scroll_content_, 0, layout.header_height);
    lv_obj_set_size(scroll_content_, layout.width, layout.height - layout.header_height);
    lv_obj_set_style_pad_left(scroll_content_, layout.content_padding_horizontal, 0);
    lv_obj_set_style_pad_right(scroll_content_, layout.content_padding_horizontal, 0);
    lv_obj_set_style_pad_top(scroll_content_, layout.content_padding_top, 0);
    lv_obj_set_style_pad_bottom(scroll_content_, layout.content_padding_bottom, 0);
    lv_obj_set_style_pad_row(scroll_content_, layout.row_gap, 0);
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
    lv_obj_set_flex_flow(scroll_content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll_content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_width(scroll_content_, layout.scrollbar_width, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll_content_, layout.scrollbar_radius, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll_content_, lv_color_hex(theme::kStrongBorder), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll_content_, LV_OPA_COVER, LV_PART_SCROLLBAR);

    DrawRow(scroll_content_, 0U, host_ui::SystemMenuItem::kWifi, LV_SYMBOL_WIFI,
            strings.Get(host_strings::Id::kSystemSettingsWifi), WifiDetail(model, strings), theme::kAccent, true);
    DrawRow(scroll_content_, 1U, host_ui::SystemMenuItem::kRemoteControl, LV_SYMBOL_REFRESH,
            strings.Get(host_strings::Id::kSystemSettingsRemoteControl), RemoteControlDetail(model, strings),
            theme::kPositive, true);
    const auto firmware_detail = FirmwareDetail(model);
    DrawRow(scroll_content_, 2U, host_ui::SystemMenuItem::kSystemInformation, "i",
            strings.Get(host_strings::Id::kSystemSettingsSystemInformation), firmware_detail.data(), theme::kAccent);
    if (firmware_update_dot_ != nullptr && !model.firmware_update_available) {
        lv_obj_add_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
    }
    DrawRow(scroll_content_, 3U, host_ui::SystemMenuItem::kLanguage, "A",
            strings.Get(host_strings::Id::kSystemSettingsLanguage),
            model.language != nullptr ? model.language : strings.Get(host_strings::Id::kLanguageEnglish),
            theme::kSuccess);
    DrawRow(scroll_content_, 4U, host_ui::SystemMenuItem::kAppearance, LV_SYMBOL_TINT,
            strings.Get(host_strings::Id::kSystemSettingsAppearance), ThemeDetail(model.theme_mode), theme::kAccent);
    DrawRow(scroll_content_, 5U, host_ui::SystemMenuItem::kPowerManagement, LV_SYMBOL_POWER,
            strings.Get(host_strings::Id::kSystemSettingsPowerManagement), PowerManagementDetail(model),
            theme::kWarning);
    DrawRow(scroll_content_, 6U, host_ui::SystemMenuItem::kManageApps, LV_SYMBOL_LIST,
            strings.Get(host_strings::Id::kSystemSettingsManageApps),
            model.installed_app_count == 1U ? "1 installed app" : "Installed apps", theme::kOrange);

    lv_obj_move_foreground(root);
    platform::lvgl::RequestDisplayRefresh(display_);
    ESP_LOGI(kTag, "System Settings visible: %" PRId32 "x%" PRId32 " wifi=%s apps=%" PRIu32, layout.width,
             layout.height, model.wifi_available ? (model.wifi_connected ? "connected" : "available") : "unavailable",
             model.installed_app_count);
    return {};
}

void SystemMenuUi::Deactivate() {
    action_sink_ = nullptr;
    action_context_ = nullptr;
    ResetObjectPointers();
    display_ = nullptr;
    layout_ = nullptr;
}

}  // namespace micropixel::host_ui::lvgl::square_common
