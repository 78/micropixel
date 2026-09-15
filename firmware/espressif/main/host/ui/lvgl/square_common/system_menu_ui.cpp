#include "host/ui/lvgl/square_common/system_menu_ui.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/fonts/language_pack_catalog.hpp"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui_internal.hpp"
#include "host_strings.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

constexpr char kTag[] = "system_menu_ui";
constexpr std::array kLanguageIds{host_strings::Id::kLanguageEnglish, host_strings::Id::kLanguageSimplifiedChinese,
                                  host_strings::Id::kLanguageTraditionalChinese, host_strings::Id::kLanguageJapanese,
                                  host_strings::Id::kLanguageKorean};
const char* LanguageStatus(const SystemMenuModel& model, const host_strings::Catalog& strings) {
    using Id = host_strings::Id;
    switch (model.language_state) {
        case LanguageDownloadState::kChecking:
            return strings.Get(Id::kLanguageChecking);
        case LanguageDownloadState::kConfirm:
            return strings.Get(model.language_updating ? Id::kLanguageUpdateDetail : Id::kLanguageConfirmDetail);
        case LanguageDownloadState::kCurrent:
            return strings.Get(Id::kLanguageFontCurrent);
        case LanguageDownloadState::kDownloading:
            return strings.Get(Id::kLanguageDownloading);
        case LanguageDownloadState::kNoSpace:
            return strings.Get(Id::kLanguageNoSpace);
        case LanguageDownloadState::kFailed:
            return strings.Get(Id::kLanguageFailed);
        case LanguageDownloadState::kApplying:
            return strings.Get(Id::kLanguageApplying);
        case LanguageDownloadState::kAppRunning:
            return strings.Get(Id::kLanguageCloseApp);
        case LanguageDownloadState::kIdle:
            return strings.Get(Id::kLanguageSubtitle);
    }
    return strings.Get(Id::kLanguageSubtitle);
}

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void StyleMenuContainer(lv_obj_t* object) {
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

std::array<char, 96> PowerManagementDetail(const host_ui::SystemMenuModel& model) {
    using Id = host_strings::Id;
    std::array<char, 96> detail{};
    const bool power_off = model.idle_power_action == device::IdlePowerAction::kPowerOff;
    if (model.auto_sleep_timeout_minutes == 0U) {
        std::snprintf(detail.data(), detail.size(), "%s",
                      UiText(power_off ? Id::kUiAutoPowerOffDisabled : Id::kUiAutoSleepDisabled));
    } else {
        std::snprintf(detail.data(), detail.size(),
                      UiText(power_off ? Id::kUiPowerOffAfterMinutes : Id::kUiSleepAfterMinutes),
                      static_cast<unsigned>(model.auto_sleep_timeout_minutes));
    }
    return detail;
}

const char* ThemeDetail(host_ui::SystemThemeMode mode) {
    using Id = host_strings::Id;
    switch (mode) {
        case host_ui::SystemThemeMode::kPureBlack:
            return UiText(Id::kUiPureBlack);
        case host_ui::SystemThemeMode::kDeepBlue:
            return UiText(Id::kUiDeepBlue);
        case host_ui::SystemThemeMode::kSoftIvory:
            return UiText(Id::kUiSoftIvory);
    }
    return UiText(Id::kUiPureBlack);
}

std::array<char, host_ui::kFirmwareUpdateMessageCapacity> FirmwareDetail(const host_ui::SystemMenuModel& model) {
    using Id = host_strings::Id;
    std::array<char, host_ui::kFirmwareUpdateMessageCapacity> detail{};
    const char* text = UiText(Id::kUiDeviceFirmwareAndMemory);
    if (model.firmware_update_state == host_ui::FirmwareUpdateState::kDownloading) {
        text = UiText(Id::kUiDownloadingFirmware);
    } else if (model.firmware_update_state == host_ui::FirmwareUpdateState::kVerifying) {
        text = UiText(Id::kUiVerifyingPackage);
    } else if (model.firmware_update_state == host_ui::FirmwareUpdateState::kInstalling) {
        text = UiText(Id::kUiInstallingFirmware);
    } else if (model.firmware_update_available) {
        std::snprintf(detail.data(), detail.size(), UiText(Id::kUiUpdateToS), model.latest_firmware_version.data());
        return detail;
    } else if (model.firmware_update_state == host_ui::FirmwareUpdateState::kCurrent) {
        text = UiText(Id::kUiFirmwareIsUpToDate);
    }
    std::snprintf(detail.data(), detail.size(), "%s", text);
    return detail;
}

}  // namespace

void SystemMenuUi::ResetObjectPointers() {
    if (language_timer_) lv_timer_delete(language_timer_);
    language_timer_ = nullptr;
    root_ = nullptr;
    language_overlay_ = nullptr;
    language_sheet_detail_ = nullptr;
    language_version_label_ = nullptr;
    language_size_label_ = nullptr;
    language_space_label_ = nullptr;
    language_progress_bar_ = nullptr;
    language_confirm_ = nullptr;
    language_cancel_ = nullptr;
    progress_reader_ = nullptr;
    shown_progress_ = 255U;
    scroll_content_ = nullptr;
    language_status_label_ = nullptr;
    wifi_detail_label_ = nullptr;
    remote_control_detail_label_ = nullptr;
    system_information_detail_label_ = nullptr;
    appearance_detail_label_ = nullptr;
    power_management_detail_label_ = nullptr;
    firmware_update_dot_ = nullptr;
    language_update_dot_ = nullptr;
    apps_update_dot_ = nullptr;
    font_update_button_ = nullptr;
    font_update_shown_ = false;
    row_bindings_ = {};
}

void SystemMenuUi::DrawRow(lv_obj_t* parent, size_t index, host_ui::SystemMenuItem item, const char* icon_text,
                           const char* name, const char* detail, uint32_t icon_color, bool emphasized_border,
                           bool interactive) {
    if (parent == nullptr || layout_ == nullptr || index >= row_bindings_.size()) {
        return;
    }
    const SystemMenuLayout& layout = *layout_;
    row_bindings_[index] = RowBinding{.ui = this, .index = static_cast<uint32_t>(index), .item = item};

    lv_obj_t* panel = interactive ? lv_button_create(parent) : lv_obj_create(parent);
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
    if (interactive) {
        lv_obj_add_event_cb(panel, RowEvent, LV_EVENT_SHORT_CLICKED, &row_bindings_[index]);
    } else {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    }
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(panel, layout.row_padding_horizontal, 0);
    lv_obj_set_style_pad_right(panel, layout.row_padding_horizontal, 0);
    lv_obj_set_style_pad_column(panel, layout.row_content_gap, 0);

    lv_obj_t* icon = CreateLabel(panel, icon_text, platform::lvgl::BuiltinLatinFont(layout.row_icon_font), icon_color);
    lv_obj_set_width(icon, layout.row_icon_width);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* text_column = lv_obj_create(panel);
    StyleMenuContainer(text_column);
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
    if (!language_view_ && (item == SystemMenuItem::kSystemInformation || item == SystemMenuItem::kLanguage ||
                            item == SystemMenuItem::kManageApps)) {
        auto* dot = lv_obj_create(panel);
        lv_obj_set_size(dot, layout.firmware_dot_size, layout.firmware_dot_size);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(theme::kNotification), 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        if (item == SystemMenuItem::kSystemInformation)
            firmware_update_dot_ = dot;
        else if (item == SystemMenuItem::kLanguage)
            language_update_dot_ = dot;
        else
            apps_update_dot_ = dot;
    }
    if (!interactive) return;
    lv_obj_t* chevron = CreateLabel(panel, LV_SYMBOL_RIGHT, platform::lvgl::BuiltinLatinFont(layout.row_name_font),
                                    theme::kPrimaryText);
    lv_obj_set_width(chevron, layout.row_chevron_width);
    lv_obj_set_style_text_align(chevron, LV_TEXT_ALIGN_RIGHT, 0);
}

void SystemMenuUi::Update(const host_ui::SystemMenuModel& model) {
    if (language_view_ && language_status_label_ && esp_lv_adapter_lock(-1) == ESP_OK) {
        UpdateFontButtonLocked(model);
        if (UpdateLanguageSheetLocked(model)) platform::lvgl::RequestDisplayRefresh(display_);
        esp_lv_adapter_unlock();
        return;
    }
    if (wifi_detail_label_ == nullptr || remote_control_detail_label_ == nullptr ||
        system_information_detail_label_ == nullptr || appearance_detail_label_ == nullptr ||
        firmware_update_dot_ == nullptr || display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    const host_strings::Catalog strings = host_strings::ForTag(model.locale);
    lv_label_set_text(wifi_detail_label_, WifiDetail(model, strings));
    lv_label_set_text(remote_control_detail_label_, RemoteControlDetail(model, strings));
    if (power_management_detail_label_ != nullptr) {
        lv_label_set_text(power_management_detail_label_, PowerManagementDetail(model).data());
    }
    lv_label_set_text(appearance_detail_label_, ThemeDetail(model.theme_mode));
    const auto firmware_detail = FirmwareDetail(model);
    lv_label_set_text(system_information_detail_label_, firmware_detail.data());
    UpdateBadgesLocked(model);
    platform::lvgl::RequestDisplayRefresh(display_);
    esp_lv_adapter_unlock();
}

void SystemMenuUi::UpdateBadgesLocked(const SystemMenuModel& model) {
    const auto show = [](lv_obj_t* dot, bool visible) {
        if (!dot) return;
        if (visible)
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    };
    show(firmware_update_dot_, model.firmware_update_available);
    show(language_update_dot_, model.font_update_available);
    show(apps_update_dot_, model.app_updates_available);
}

void SystemMenuUi::UpdateFontButtonLocked(const SystemMenuModel& model) {
    if (!font_update_button_ || font_update_shown_ == model.font_update_available) return;
    font_update_shown_ = model.font_update_available;
    if (font_update_shown_)
        lv_obj_remove_flag(font_update_button_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(font_update_button_, LV_OBJ_FLAG_HIDDEN);
    platform::lvgl::RequestDisplayRefresh(display_);
}

void SystemMenuUi::UpdateFontEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemMenuUi*>(lv_event_get_user_data(event));
    if (ui && ui->action_sink_ && ui->font_update_shown_)
        ui->action_sink_(ui->action_context_, {.type = SystemUiActionType::kUpdateLanguageFont});
}

void SystemMenuUi::LanguageSheetDeleted(lv_event_t* event) {
    auto* ui = static_cast<SystemMenuUi*>(lv_event_get_user_data(event));
    if (!ui || ui->language_overlay_ != lv_event_get_target_obj(event)) return;
    if (ui->language_timer_) lv_timer_delete(ui->language_timer_);
    ui->language_timer_ = nullptr;
    ui->language_overlay_ = nullptr;
}

void SystemMenuUi::LanguageProgressTimer(lv_timer_t* timer) {
    auto* ui = static_cast<SystemMenuUi*>(lv_timer_get_user_data(timer));
    if (ui) ui->UpdateLanguageProgressLocked();
}

void SystemMenuUi::UpdateLanguageProgressLocked() {
    if (!language_overlay_ || !progress_reader_ ||
        (language_state_ != LanguageDownloadState::kDownloading && language_state_ != LanguageDownloadState::kApplying))
        return;
    const uint8_t value = progress_reader_(progress_context_);
    if (shown_progress_ == value) return;
    shown_progress_ = value;
    lv_bar_set_value(language_progress_bar_, value, LV_ANIM_OFF);
    lv_label_set_text_fmt(language_sheet_detail_, "%s %u%%", value < 70U ? download_text_ : install_text_, value);
    platform::lvgl::RequestDisplayRefresh(display_);
}

bool SystemMenuUi::UpdateLanguageSheetLocked(const host_ui::SystemMenuModel& model) {
    using system_detail_internal::CreateActionSheet;
    using system_detail_internal::Label;
    using Id = host_strings::Id;
    if (!model.language_sheet) {
        const bool closed = language_overlay_ != nullptr;
        if (language_timer_) lv_timer_delete(language_timer_);
        language_timer_ = nullptr;
        if (language_overlay_) lv_obj_delete(language_overlay_);
        language_overlay_ = nullptr;
        return closed;
    }
    const auto strings = host_strings::ForTag(model.locale);
    const bool created = !language_overlay_;
    if (!created && rendered_language_state_ == model.language_state &&
        rendered_download_bytes_ == model.language_download_bytes &&
        rendered_required_bytes_ == model.language_required_bytes && rendered_free_bytes_ == model.language_free_bytes)
        return false;
    rendered_language_state_ = model.language_state;
    rendered_download_bytes_ = model.language_download_bytes;
    rendered_required_bytes_ = model.language_required_bytes;
    rendered_free_bytes_ = model.language_free_bytes;
    language_state_ = model.language_state;
    progress_reader_ = model.language_progress_reader;
    progress_context_ = model.language_progress_context;
    download_text_ = strings.Get(Id::kLanguageDownloading);
    install_text_ = strings.Get(Id::kLanguageApplying);
    if (created) {
        auto* sheet = CreateActionSheet(presenter_, action_sink_, action_context_, page_layout_, root_,
                                        CancelLanguageEvent, this, theme::kStrongBorder, &language_overlay_);
        lv_obj_add_event_cb(language_overlay_, LanguageSheetDeleted, LV_EVENT_DELETE, this);
        lv_obj_set_style_max_height(sheet, page_layout_.height - page_layout_.safe_horizontal * 2, 0);
        lv_obj_add_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
        (void)Label(sheet, strings.Get(model.language_updating ? Id::kLanguageUpdateTitle : Id::kLanguageConfirmTitle),
                    platform::lvgl::SystemFontRole::kLarge, theme::kPrimaryText);
        (void)Label(sheet,
                    strings.Get(kLanguageIds[std::min<size_t>(model.language_selected, kLanguageIds.size() - 1U)]),
                    platform::lvgl::SystemFontRole::kLarge, theme::kSuccess);
        if (model.language_updating) {
            language_version_label_ = Label(sheet, "", platform::lvgl::SystemFontRole::kSmall, theme::kSecondaryText);
        }
        language_sheet_detail_ = Label(sheet, "", platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
        lv_obj_set_width(language_sheet_detail_, LV_PCT(100));
        lv_label_set_long_mode(language_sheet_detail_, LV_LABEL_LONG_WRAP);
        // Reserve two lines and both data rows before the slide snapshot.
        // Discovery changes their text, not the sheet height or button position.
        const auto* detail_font = platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kMedium);
        lv_obj_set_height(language_sheet_detail_, detail_font->line_height * 2U);
        language_size_label_ = Label(sheet, "", platform::lvgl::SystemFontRole::kMedium, theme::kPrimaryText);
        lv_obj_set_width(language_size_label_, LV_PCT(100));
        lv_label_set_long_mode(language_size_label_, LV_LABEL_LONG_DOT);
        language_space_label_ = Label(sheet, "", platform::lvgl::SystemFontRole::kMedium, theme::kSecondaryText);
        lv_obj_set_width(language_space_label_, LV_PCT(100));
        lv_label_set_long_mode(language_space_label_, LV_LABEL_LONG_DOT);
        language_progress_bar_ = lv_bar_create(sheet);
        lv_obj_set_size(language_progress_bar_, LV_PCT(100), 6);
        lv_bar_set_range(language_progress_bar_, 0, 100);
        lv_obj_set_style_bg_color(language_progress_bar_, lv_color_hex(theme::kSuccess), LV_PART_INDICATOR);
        language_confirm_ = CreateSystemActionButton(
            sheet, page_layout_,
            strings.Get(model.language_updating ? Id::kLanguageUpdateFont : Id::kLanguageConfirmAction),
            theme::kSuccess, false);
        lv_obj_add_event_cb(language_confirm_, ConfirmLanguageEvent, LV_EVENT_SHORT_CLICKED, this);
        language_cancel_ =
            CreateSystemActionButton(sheet, page_layout_, strings.Get(Id::kLanguageClose), theme::kPrimaryText, false);
        lv_obj_add_event_cb(language_cancel_, CancelLanguageEvent, LV_EVENT_SHORT_CLICKED, this);
        language_timer_ = lv_timer_create(LanguageProgressTimer, 100U, this);
    }
    if (model.language_updating && language_version_label_)
        lv_label_set_text_fmt(language_version_label_, "%s -> %s",
                              model.font_current_version ? model.font_current_version : "",
                              model.font_update_version ? model.font_update_version : "");
    const bool busy = model.language_state == LanguageDownloadState::kDownloading ||
                      model.language_state == LanguageDownloadState::kApplying;
    const bool checking = model.language_state == LanguageDownloadState::kChecking;
    const auto hidden = [](lv_obj_t* object, bool value) {
        if (value)
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    };
    hidden(language_cancel_, busy);
    hidden(language_confirm_, busy);
    hidden(language_progress_bar_, !busy);
    if (model.language_state == LanguageDownloadState::kConfirm)
        lv_obj_remove_state(language_confirm_, LV_STATE_DISABLED);
    else
        lv_obj_add_state(language_confirm_, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(
        lv_obj_get_child(language_confirm_, 0),
        lv_color_hex(model.language_state == LanguageDownloadState::kConfirm ? theme::kSuccess : theme::kMutedText), 0);
    const bool unknown_size = checking || model.language_state == LanguageDownloadState::kAppRunning ||
                              model.language_state == LanguageDownloadState::kFailed;
    if (unknown_size) {
        lv_label_set_text_fmt(language_size_label_, "%s: --", strings.Get(Id::kLanguageFontSize));
        lv_label_set_text_fmt(language_space_label_, "%s: --", strings.Get(Id::kLanguageStorage));
    } else {
        // Show storage required for atomic installation alongside actual current free space.
        lv_label_set_text_fmt(language_size_label_, "%s: %lu KiB", strings.Get(Id::kLanguageFontSize),
                              static_cast<unsigned long>((model.language_download_bytes + 1023U) / 1024U));
        lv_label_set_text_fmt(language_space_label_, "%s: %lu / %lu KiB", strings.Get(Id::kLanguageStorage),
                              static_cast<unsigned long>((model.language_required_bytes + 1023U) / 1024U),
                              static_cast<unsigned long>(model.language_free_bytes / 1024U));
    }
    if (!busy) {
        lv_label_set_text(language_sheet_detail_,
                          model.language_state == LanguageDownloadState::kConfirm && model.language_download_bytes == 0U
                              ? strings.Get(Id::kLanguageInstalled)
                              : LanguageStatus(model, strings));
        shown_progress_ = 255U;
    } else
        UpdateLanguageProgressLocked();
    return true;
}

void SystemMenuUi::ConfirmLanguageEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemMenuUi*>(lv_event_get_user_data(event));
    if (!ui || !ui->action_sink_ || ui->language_state_ != LanguageDownloadState::kConfirm) return;
    // Reject duplicate taps before the Host consumes the first confirmation.
    ui->language_state_ = LanguageDownloadState::kDownloading;
    lv_obj_add_state(ui->language_confirm_, LV_STATE_DISABLED);
    ui->action_sink_(ui->action_context_, {.type = SystemUiActionType::kConfirmLanguage});
}

void SystemMenuUi::CancelLanguageEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemMenuUi*>(lv_event_get_user_data(event));
    if (!ui || !ui->action_sink_ || ui->language_state_ == LanguageDownloadState::kDownloading ||
        ui->language_state_ == LanguageDownloadState::kApplying)
        return;
    ui->action_sink_(ui->action_context_, {.type = SystemUiActionType::kCancelLanguage});
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
        binding->ui->action_sink_(
            binding->ui->action_context_,
            host_ui::SystemUiAction{
                .type = binding->ui->language_view_ ? host_ui::SystemUiActionType::kSelectLanguage
                                                    : host_ui::SystemUiActionType::kSelectSystemMenuItem,
                .value = binding->ui->language_view_ ? binding->index : static_cast<uint32_t>(binding->item)});
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
    language_view_ = model.language_view;
    root_ = root;
    const host_strings::Catalog strings = host_strings::ForTag(model.locale);

    lv_obj_t* header = lv_obj_create(root);
    StyleMenuContainer(header);
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
    StyleMenuContainer(header_text);
    lv_obj_set_height(header_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(header_text, 1);
    lv_obj_set_flex_flow(header_text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(header_text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(header_text, layout.header_text_gap, 0);
    lv_obj_t* title =
        CreateLabel(header_text,
                    strings.Get(model.language_view ? host_strings::Id::kSystemSettingsLanguage
                                                    : host_strings::Id::kSystemSettingsTitle),
                    platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kTitle), theme::kPrimaryText);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* subtitle = CreateLabel(
        header_text,
        model.language_view ? LanguageStatus(model, strings) : strings.Get(host_strings::Id::kSystemSettingsSubtitle),
        platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kMedium), theme::kSecondaryText);
    lv_obj_set_width(subtitle, LV_PCT(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);

    if (model.language_view) language_status_label_ = subtitle;

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

    if (model.language_view) {
        const auto section = [&](const char* text, bool separated) {
            auto* label = CreateLabel(scroll_content_, text,
                                      platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kSmall),
                                      theme::kSecondaryText);
            lv_obj_set_width(label, LV_PCT(100));
            if (separated) lv_obj_set_style_pad_top(label, layout.row_gap * 2, 0);
        };
        section(strings.Get(host_strings::Id::kLanguageCurrent), false);
        for (size_t i = 0; i < kLanguageIds.size(); ++i) {
            if (std::string_view(model.locale) != host::fonts::kPacks[i].locale) continue;
            DrawRow(scroll_content_, i, SystemMenuItem::kLanguage, LV_SYMBOL_OK, strings.Get(kLanguageIds[i]),
                    strings.Get(host_strings::Id::kLanguageActive), theme::kSuccess, true, false);
        }
        font_update_button_ = CreateSystemActionButton(
            scroll_content_, page_layout_, strings.Get(host_strings::Id::kLanguageUpdateFont), theme::kSuccess, false);
        lv_obj_add_flag(font_update_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(font_update_button_, UpdateFontEvent, LV_EVENT_SHORT_CLICKED, this);
        UpdateFontButtonLocked(model);
        section(strings.Get(host_strings::Id::kLanguageOther), true);
        for (size_t i = 0; i < kLanguageIds.size(); ++i) {
            if (std::string_view(model.locale) == host::fonts::kPacks[i].locale) continue;
            DrawRow(scroll_content_, i, SystemMenuItem::kLanguage, "A", strings.Get(kLanguageIds[i]),
                    strings.Get(host_strings::Id::kLanguageReady), theme::kSecondaryText);
        }
        UpdateLanguageSheetLocked(model);
    } else {
        DrawRow(scroll_content_, 0U, host_ui::SystemMenuItem::kWifi, LV_SYMBOL_WIFI,
                strings.Get(host_strings::Id::kSystemSettingsWifi), WifiDetail(model, strings), theme::kAccent, true);
        DrawRow(scroll_content_, 1U, host_ui::SystemMenuItem::kRemoteControl, LV_SYMBOL_REFRESH,
                strings.Get(host_strings::Id::kSystemSettingsRemoteControl), RemoteControlDetail(model, strings),
                theme::kPositive, true);
        const auto firmware_detail = FirmwareDetail(model);
        DrawRow(scroll_content_, 2U, host_ui::SystemMenuItem::kSystemInformation, "i",
                strings.Get(host_strings::Id::kSystemSettingsSystemInformation), firmware_detail.data(),
                theme::kAccent);
        if (firmware_update_dot_ != nullptr && !model.firmware_update_available) {
            lv_obj_add_flag(firmware_update_dot_, LV_OBJ_FLAG_HIDDEN);
        }
        DrawRow(scroll_content_, 3U, host_ui::SystemMenuItem::kLanguage, "A",
                strings.Get(host_strings::Id::kSystemSettingsLanguage), UiLocaleName(model.locale), theme::kSuccess);
        DrawRow(scroll_content_, 4U, host_ui::SystemMenuItem::kAppearance, LV_SYMBOL_TINT,
                strings.Get(host_strings::Id::kSystemSettingsAppearance), ThemeDetail(model.theme_mode),
                theme::kAccent);
        if (model.idle_power_action != device::IdlePowerAction::kDisabled) {
            DrawRow(scroll_content_, 5U, host_ui::SystemMenuItem::kPowerManagement, LV_SYMBOL_POWER,
                    strings.Get(host_strings::Id::kSystemSettingsPowerManagement), PowerManagementDetail(model).data(),
                    theme::kWarning);
        }
        DrawRow(scroll_content_, 6U, host_ui::SystemMenuItem::kManageApps, LV_SYMBOL_LIST,
                strings.Get(host_strings::Id::kSystemSettingsManageApps),
                UiText(host_strings::Id::kUiInstalledAppsAndStorage), theme::kOrange);
    }

    UpdateBadgesLocked(model);
    lv_obj_move_foreground(root);
    platform::lvgl::RequestDisplayRefresh(display_);
    ESP_LOGI(kTag, "System Settings visible: %" PRId32 "x%" PRId32 " wifi=%s apps=%" PRIu32, layout.width,
             layout.height, model.wifi_available ? (model.wifi_connected ? "connected" : "available") : "unavailable",
             model.installed_app_count);
    return {};
}

void SystemMenuUi::Deactivate() {
    if (esp_lv_adapter_lock(-1) != ESP_OK) return;
    action_sink_ = nullptr;
    action_context_ = nullptr;
    ResetObjectPointers();
    display_ = nullptr;
    layout_ = nullptr;
    esp_lv_adapter_unlock();
}

}  // namespace micropixel::host_ui::lvgl::square_common
