#include "platform/lvgl/ui/square_720/system_detail_ui.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl::square_720 {
namespace {

constexpr char kTag[] = "micropixel_details";
constexpr int32_t kScreenWidth = 720;
constexpr int32_t kScreenHeight = 720;
constexpr int32_t kAppManagementRowTop = 90;
constexpr int32_t kAppManagementRowStep = 116;
constexpr int32_t kAppManagementRowHeight = 104;
constexpr uint32_t kAppManagementRowOverscan = 1U;
constexpr std::array<uint8_t, 4> kAutoSleepTimeoutMinutes{1U, 5U, 10U, 30U};

struct Bounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int32_t x, int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* CreatePanel(lv_obj_t* parent, const Bounds& bounds, uint32_t background, uint32_t border, int32_t radius) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, bounds.x, bounds.y);
    lv_obj_set_size(panel, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, radius, 0);
    lv_obj_set_style_border_width(panel, border == 0U ? 0 : 1, 0);
    if (border != 0U) {
        lv_obj_set_style_border_color(panel, lv_color_hex(border), 0);
    }
    lv_obj_set_style_bg_color(panel, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

void CreateDismissibleScrim(lv_obj_t* parent, lv_event_cb_t dismiss_callback, void* user_data) {
    lv_obj_t* scrim =
        CreatePanel(parent, Bounds{.x = 0, .y = 0, .width = kScreenWidth, .height = kScreenHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, dismiss_callback, LV_EVENT_SHORT_CLICKED, user_data);
}

lv_obj_t* CreateActionButton(lv_obj_t* parent, const Bounds& bounds, const char* text, uint32_t color,
                             uint32_t border = 0x365472U) {
    constexpr lv_style_selector_t kPressed = static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    lv_obj_t* button = CreatePanel(parent, bounds, 0x16263aU, border, 16);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0b1726U), kPressed);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, BuiltinLatinFont(SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_center(label);
    return button;
}

void FormatInformationSize(uint32_t kib, char* buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0U) {
        return;
    }
    if (kib >= 1024U) {
        const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(kib) * 10U + 512U) / 1024U);
        std::snprintf(buffer, capacity, "%" PRIu32 ".%" PRIu32 " MB", tenths / 10U, tenths % 10U);
    } else {
        std::snprintf(buffer, capacity, "%" PRIu32 " KB", kib);
    }
}

lv_obj_t* CreateDetailScrollContent(lv_obj_t* parent, int32_t content_height, lv_event_cb_t scroll_callback,
                                    void* user_data) {
    lv_obj_t* scroll = lv_obj_create(parent);
    lv_obj_set_pos(scroll, 0, 108);
    lv_obj_set_size(scroll, kScreenWidth, kScreenHeight - 108);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    constexpr lv_obj_flag_t kScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(scroll, kScrollFlags);
    lv_obj_add_event_cb(scroll, scroll_callback, LV_EVENT_SCROLL, user_data);
    lv_obj_add_event_cb(scroll, scroll_callback, LV_EVENT_SCROLL_END, user_data);
    lv_obj_set_style_width(scroll, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(scroll, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll, lv_color_hex(0x42607fU), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_COVER, LV_PART_SCROLLBAR);

    lv_obj_t* bottom_spacer = lv_obj_create(scroll);
    lv_obj_set_pos(bottom_spacer, 0, content_height - 1);
    lv_obj_set_size(bottom_spacer, 1, 1);
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);
    return scroll;
}

lv_obj_t* DrawDetailHeader(lv_obj_t* root, const char* title, const char* subtitle, lv_event_cb_t back_callback,
                           void* user_data) {
    lv_obj_t* back = CreatePanel(root, Bounds{.x = 40, .y = 32, .width = 56, .height = 56}, 0x0d1929U, 0x42607fU, 18);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(back, back_callback, LV_EVENT_SHORT_CLICKED, user_data);
    lv_obj_t* back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, BuiltinLatinFont(SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_center(back_icon);
    (void)CreateLabel(root, title, BuiltinLatinFont(SystemFontRole::kTitle), 0xf2f7ffU, 116, 28);
    if (subtitle != nullptr && subtitle[0] != '\0') {
        (void)CreateLabel(root, subtitle, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 116, 70);
    }
    return back;
}

void DrawInformationSectionLabel(lv_obj_t* parent, const char* text, int32_t y) {
    (void)CreateLabel(parent, text, BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, 42, y);
}

void DrawInformationRow(lv_obj_t* panel, int32_t y, const char* label, const char* value) {
    if (y > 0) {
        (void)CreatePanel(panel, Bounds{.x = 20, .y = y, .width = 600, .height = 1}, 0x21364eU, 0U, 0);
    }
    (void)CreateLabel(panel, label, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 22, y + 18);
    lv_obj_t* value_label =
        CreateLabel(panel, value, BuiltinLatinFont(SystemFontRole::kMedium), 0xf2f7ffU, 260, y + 18);
    lv_obj_set_width(value_label, 356);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
}

void DrawMemoryMetric(lv_obj_t* panel, int32_t x, int32_t y, const char* name, uint32_t kib, uint32_t color) {
    char value[24]{};
    FormatInformationSize(kib, value, sizeof(value));
    (void)CreateLabel(panel, name, BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, x, y);
    lv_obj_t* label = CreateLabel(panel, value, BuiltinLatinFont(SystemFontRole::kMedium), color, x, y + 30);
    lv_obj_set_width(label, 106);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void DrawMemoryStatisticsRow(lv_obj_t* panel, int32_t y, const char* name,
                             const host_ui::MemoryStatisticsModel& memory) {
    if (y > 0) {
        (void)CreatePanel(panel, Bounds{.x = 20, .y = y, .width = 600, .height = 1}, 0x21364eU, 0U, 0);
    }
    (void)CreateLabel(panel, name, BuiltinLatinFont(SystemFontRole::kMedium), 0xf2f7ffU, 22, y + 35);
    DrawMemoryMetric(panel, 176, y + 17, "TOTAL", memory.total_kib, 0xf2f7ffU);
    DrawMemoryMetric(panel, 288, y + 17, "FREE", memory.free_kib, 0xf2f7ffU);
    DrawMemoryMetric(panel, 400, y + 17, "MINIMUM", memory.minimum_free_kib, 0x69a7ffU);
    DrawMemoryMetric(panel, 512, y + 17, "LARGEST", memory.largest_free_block_kib, 0xf2f7ffU);
}

void DrawAppMoreIndicator(lv_obj_t* parent) {
    for (int32_t index = 0; index < 3; ++index) {
        (void)CreatePanel(parent, Bounds{.x = 594, .y = 35 + index * 13, .width = 7, .height = 7}, 0xa9bdd5U, 0U,
                          LV_RADIUS_CIRCLE);
    }
}

const char* RemoteControlStateText(host_ui::RemoteControlConnectionState state) {
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

uint32_t RemoteControlStateColor(host_ui::RemoteControlConnectionState state) {
    switch (state) {
        case host_ui::RemoteControlConnectionState::kConnected:
            return 0xc5f36dU;
        case host_ui::RemoteControlConnectionState::kConnecting:
        case host_ui::RemoteControlConnectionState::kWaitingForNetwork:
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

void DrawFirmwareUpdateView(lv_obj_t* root, const host_ui::SystemInformationModel& model, lv_event_cb_t back_callback,
                            lv_event_cb_t update_callback, void* user_data) {
    const bool in_progress = FirmwareUpdateInProgress(model.firmware_update_state);
    lv_obj_t* back = DrawDetailHeader(root, "Firmware Update", "Keep the device powered on", back_callback, user_data);
    if (in_progress) {
        lv_obj_add_state(back, LV_STATE_DISABLED);
        lv_obj_set_style_opa(back, LV_OPA_30, 0);
    }

    lv_obj_t* panel =
        CreatePanel(root, Bounds{.x = 40, .y = 138, .width = 640, .height = 500}, 0x111f32U, 0x2e4562U, 26);
    lv_obj_t* icon = CreateLabel(
        panel, LV_SYMBOL_REFRESH, BuiltinLatinFont(SystemFontRole::kTitle),
        model.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? 0xff6b74U : 0x69a7ffU, 292, 38);
    lv_obj_set_width(icon, 56);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);

    const char* stage = FirmwareUpdateStageText(model.firmware_update_state);
    lv_obj_t* stage_label = CreateLabel(panel, stage, BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 40, 99);
    lv_obj_set_width(stage_label, 560);
    lv_obj_set_style_text_align(stage_label, LV_TEXT_ALIGN_CENTER, 0);

    char versions[160]{};
    const char* latest = DisplayText(model.latest_firmware_version.data(), model.firmware_version.data());
    std::snprintf(versions, sizeof(versions), "%s  %s  %s", model.firmware_version.data(), LV_SYMBOL_RIGHT, latest);
    lv_obj_t* version_label =
        CreateLabel(panel, versions, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 40, 141);
    lv_obj_set_width(version_label, 560);
    lv_obj_set_style_text_align(version_label, LV_TEXT_ALIGN_CENTER, 0);

    const uint8_t progress = std::min<uint8_t>(model.firmware_progress_percent, 100U);
    char percent[16]{};
    std::snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(progress));
    lv_obj_t* percent_label = CreateLabel(panel, percent, BuiltinLatinFont(SystemFontRole::kTitle), 0xf2f7ffU, 40, 190);
    lv_obj_set_width(percent_label, 560);
    lv_obj_set_style_text_align(percent_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* progress_bar = lv_bar_create(panel);
    lv_obj_set_pos(progress_bar, 48, 244);
    lv_obj_set_size(progress_bar, 544, 18);
    lv_obj_set_style_radius(progress_bar, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x21364eU), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_bar, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x69a7ffU), LV_PART_INDICATOR);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, progress, LV_ANIM_OFF);

    if (model.firmware_size_bytes != 0U) {
        char size[64]{};
        const uint32_t shown_bytes = std::min(model.firmware_processed_bytes, model.firmware_size_bytes);
        const uint32_t shown_tenths =
            static_cast<uint32_t>((static_cast<uint64_t>(shown_bytes) * 10U) / (1024U * 1024U));
        const uint32_t total_tenths =
            static_cast<uint32_t>((static_cast<uint64_t>(model.firmware_size_bytes) * 10U) / (1024U * 1024U));
        std::snprintf(size, sizeof(size), "%" PRIu32 ".%" PRIu32 " / %" PRIu32 ".%" PRIu32 " MB", shown_tenths / 10U,
                      shown_tenths % 10U, total_tenths / 10U, total_tenths % 10U);
        lv_obj_t* size_label = CreateLabel(panel, size, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 40, 281);
        lv_obj_set_width(size_label, 560);
        lv_obj_set_style_text_align(size_label, LV_TEXT_ALIGN_CENTER, 0);
    }

    const char* message = DisplayText(model.firmware_update_message.data(), stage);
    lv_obj_t* message_label = CreateLabel(
        panel, message, BuiltinLatinFont(SystemFontRole::kMedium),
        model.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? 0xff8a91U : 0x91a4bdU, 40, 326);
    lv_obj_set_width(message_label, 560);
    lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);

    if (!in_progress && model.firmware_update_installable) {
        const char* button_text = model.firmware_update_state == host_ui::FirmwareUpdateState::kFailed ? "Retry update"
                                  : model.firmware_update_available ? "Download and install"
                                                                    : "Reinstall current version";
        lv_obj_t* update = CreateActionButton(panel, Bounds{.x = 160, .y = 402, .width = 320, .height = 64},
                                              button_text, 0xcdf5c6U, 0x5e9f59U);
        lv_obj_add_event_cb(update, update_callback, LV_EVENT_SHORT_CLICKED, user_data);
    }
}

}  // namespace

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowSystemInformationLocked(
    lv_obj_t* root, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    LeaveRemoteControl();
    LeaveAppManagement();
    LeavePowerManagement();
    root_ = root;
    system_information_action_sink_ = action_sink;
    system_information_action_context_ = action_context;
    active_screen_ = Screen::kSystemInformation;
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    if (model.firmware_update_view) {
        DrawFirmwareUpdateView(root_, model, SystemInformationBackEvent, SystemInformationUpdateEvent, this);
        lv_obj_move_foreground(root_);
        RequestDisplayRefresh(lv_obj_get_display(root_));
        ESP_LOGI(kTag, "Firmware Update visible: state=%u progress=%u%%",
                 static_cast<unsigned>(model.firmware_update_state),
                 static_cast<unsigned>(model.firmware_progress_percent));
        return {};
    }
    (void)DrawDetailHeader(root_, "System Information", "Device and software", SystemInformationBackEvent, this);

    constexpr int32_t kContentHeight = 1420;
    lv_obj_t* scroll_content = CreateDetailScrollContent(root_, kContentHeight, DetailScrollEvent, this);
    lv_obj_t* firmware =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 14, .width = 640, .height = 190}, 0x111f32U, 0x2e4562U, 22);
    (void)CreateLabel(firmware, "MicroPixel Firmware", BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 18);
    char version[96]{};
    std::snprintf(version, sizeof(version), "Version %s", model.firmware_version.data());
    (void)CreateLabel(firmware, version, BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 49);
    char build[224]{};
    std::snprintf(build, sizeof(build), "Build %s   %s %s", model.build_id.data(), model.build_date.data(),
                  model.build_time.data());
    lv_obj_t* build_label = CreateLabel(firmware, build, BuiltinLatinFont(SystemFontRole::kMedium), 0x69a7ffU, 24, 88);
    lv_obj_set_width(build_label, 592);
    lv_label_set_long_mode(build_label, LV_LABEL_LONG_DOT);

    const char* update_status = model.firmware_update_message[0] != '\0' ? model.firmware_update_message.data()
                                                                         : "Connect to Control to check for updates";
    lv_obj_t* update_status_label =
        CreateLabel(firmware, update_status, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 139);
    lv_obj_set_width(update_status_label, model.firmware_update_installable ? 382 : 592);
    lv_label_set_long_mode(update_status_label, LV_LABEL_LONG_DOT);
    if (model.firmware_update_installable) {
        char update_action[96]{};
        std::snprintf(update_action, sizeof(update_action),
                      model.firmware_update_available ? "Update to %s" : "Reinstall %s",
                      model.latest_firmware_version.data());
        lv_obj_t* update = CreateActionButton(firmware, Bounds{.x = 430, .y = 124, .width = 184, .height = 50},
                                              update_action, 0xcdf5c6U, 0x5e9f59U);
        lv_obj_add_event_cb(update, SystemInformationUpdateEvent, LV_EVENT_SHORT_CLICKED, this);
    }

    DrawInformationSectionLabel(scroll_content, "MEMORY", 228);
    lv_obj_t* memory =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 260, .width = 640, .height = 194}, 0x111f32U, 0x2e4562U, 22);
    DrawMemoryStatisticsRow(memory, 0, "SRAM", model.internal_sram);
    DrawMemoryStatisticsRow(memory, 97, "PSRAM", model.psram);

    DrawInformationSectionLabel(scroll_content, "HARDWARE", 482);
    lv_obj_t* hardware =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 514, .width = 640, .height = 290}, 0x111f32U, 0x2e4562U, 22);
    DrawInformationRow(hardware, 0, "Host Chip", model.host_chip.data());
    DrawInformationRow(hardware, 58, "CPU", model.cpu.data());
    DrawInformationRow(hardware, 116, "Wi-Fi Coprocessor", model.wifi_coprocessor.data());
    DrawInformationRow(hardware, 174, "Wi-Fi MAC", model.wifi_mac.data());
    DrawInformationRow(hardware, 232, "Flash", model.flash_capacity.data());

    DrawInformationSectionLabel(scroll_content, "DISPLAY", 832);
    lv_obj_t* display_panel =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 864, .width = 640, .height = 232}, 0x111f32U, 0x2e4562U, 22);
    DrawInformationRow(display_panel, 0, "Panel", model.panel.data());
    DrawInformationRow(display_panel, 58, "Interface", model.display_interface.data());
    DrawInformationRow(display_panel, 116, "Resolution", model.resolution.data());
    DrawInformationRow(display_panel, 174, "Touch", model.touch_controller.data());

    DrawInformationSectionLabel(scroll_content, "SOFTWARE", 1124);
    lv_obj_t* software =
        CreatePanel(scroll_content, Bounds{.x = 40, .y = 1156, .width = 640, .height = 232}, 0x111f32U, 0x2e4562U, 22);
    DrawInformationRow(software, 0, "ESP-IDF", model.idf_version.data());
    DrawInformationRow(software, 58, "Uptime", model.uptime.data());
    DrawInformationRow(software, 116, "Last Reset", model.last_reset.data());
    DrawInformationRow(software, 174, "Build Date", model.build_date.data());

    lv_obj_update_layout(scroll_content);
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
    ESP_LOGI(kTag, "System Information visible: chip=%s firmware=%s", model.host_chip.data(),
             model.firmware_version.data());
    return {};
}

void SystemDetailUi::UpdateSystemInformationLocked(const host_ui::SystemInformationModel& model) {
    if (!SystemInformationVisible() || root_ == nullptr) {
        return;
    }
    lv_obj_t* root = root_;
    host_ui::SystemUiActionSink action_sink = system_information_action_sink_;
    void* action_context = system_information_action_context_;
    lv_obj_clean(root);
    (void)ShowSystemInformationLocked(root, model, action_sink, action_context);
}

void SystemDetailUi::LeaveSystemInformation() {
    system_information_action_sink_ = nullptr;
    system_information_action_context_ = nullptr;
    if (active_screen_ == Screen::kSystemInformation) {
        active_screen_ = Screen::kNone;
        root_ = nullptr;
    }
}

bool SystemDetailUi::SystemInformationVisible() const { return active_screen_ == Screen::kSystemInformation; }

void* SystemDetailUi::SystemInformationActionContext() const { return system_information_action_context_; }

void SystemDetailUi::DetailScrollEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->root_ != nullptr) {
        if (ui->AppManagementVisible()) {
            ui->RefreshAppManagementRowsLocked();
        }
        RequestDisplayRefresh(lv_obj_get_display(ui->root_));
    }
}

void SystemDetailUi::SystemInformationBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->system_information_action_sink_ != nullptr) {
        ui->system_information_action_sink_(
            ui->system_information_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseSystemInformation});
    }
}

void SystemDetailUi::SystemInformationUpdateEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->system_information_action_sink_ != nullptr) {
        ui->system_information_action_sink_(
            ui->system_information_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kInstallFirmwareUpdate});
    }
}

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowPowerManagementLocked(
    lv_obj_t* root, const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    LeaveSystemInformation();
    LeaveRemoteControl();
    LeaveAppManagement();
    root_ = root;
    power_management_action_sink_ = action_sink;
    power_management_action_context_ = action_context;
    active_screen_ = Screen::kPowerManagement;
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    (void)DrawDetailHeader(root_, "Power Management", "Battery and idle behavior", PowerManagementBackEvent, this);

    lv_obj_t* auto_sleep =
        CreatePanel(root_, Bounds{.x = 40, .y = 138, .width = 640, .height = 154}, 0x111f32U, 0x2e4562U, 22);
    (void)CreateLabel(auto_sleep, "Auto sleep", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 23);
    lv_obj_t* description = CreateLabel(auto_sleep, "Sleep after no interaction while on battery power",
                                        BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 63);
    lv_obj_set_width(description, 480);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    power_management_switch_ = lv_switch_create(auto_sleep);
    lv_obj_set_pos(power_management_switch_, 526, 43);
    lv_obj_set_size(power_management_switch_, 82, 44);
    lv_obj_add_event_cb(power_management_switch_, PowerManagementSwitchEvent, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* timeout =
        CreatePanel(root_, Bounds{.x = 40, .y = 316, .width = 640, .height = 170}, 0x111f32U, 0x2e4562U, 22);
    (void)CreateLabel(timeout, "Idle timeout", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 22);
    (void)CreateLabel(timeout, "Choose how long the device waits before sleeping",
                      BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 61);
    power_management_timeout_dropdown_ = lv_dropdown_create(timeout);
    lv_obj_set_pos(power_management_timeout_dropdown_, 24, 101);
    lv_obj_set_size(power_management_timeout_dropdown_, 592, 52);
    lv_dropdown_set_options(power_management_timeout_dropdown_, "1 minute\n5 minutes\n10 minutes\n30 minutes");
    lv_obj_set_style_text_font(power_management_timeout_dropdown_, BuiltinLatinFont(SystemFontRole::kMedium), 0);
    lv_obj_add_event_cb(power_management_timeout_dropdown_, PowerManagementTimeoutEvent, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* note = CreateLabel(root_, "External power pauses the idle timer. Unplugging starts a fresh countdown.",
                                 BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, 52, 520);
    lv_obj_set_width(note, 616);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    UpdatePowerManagementLocked(model);
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
    ESP_LOGI(kTag, "Power Management visible: auto-sleep=%u min",
             static_cast<unsigned>(model.auto_sleep_timeout_minutes));
    return {};
}

void SystemDetailUi::UpdatePowerManagementLocked(const host_ui::PowerManagementModel& model) {
    if (!PowerManagementVisible() || power_management_switch_ == nullptr ||
        power_management_timeout_dropdown_ == nullptr) {
        return;
    }
    power_management_updating_ = true;
    const bool enabled = model.auto_sleep_timeout_minutes != 0U;
    if (enabled) {
        lv_obj_add_state(power_management_switch_, LV_STATE_CHECKED);
        lv_obj_remove_state(power_management_timeout_dropdown_, LV_STATE_DISABLED);
        lv_obj_set_style_opa(power_management_timeout_dropdown_, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_state(power_management_switch_, LV_STATE_CHECKED);
        lv_obj_add_state(power_management_timeout_dropdown_, LV_STATE_DISABLED);
        lv_obj_set_style_opa(power_management_timeout_dropdown_, LV_OPA_40, 0);
    }
    const uint8_t selected_minutes =
        enabled ? model.auto_sleep_timeout_minutes : host_ui::kDefaultAutoSleepTimeoutMinutes;
    uint32_t selected = 1U;
    for (uint32_t index = 0U; index < kAutoSleepTimeoutMinutes.size(); ++index) {
        if (kAutoSleepTimeoutMinutes[index] == selected_minutes) {
            selected = index;
            break;
        }
    }
    lv_dropdown_set_selected(power_management_timeout_dropdown_, selected);
    power_management_updating_ = false;
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::LeavePowerManagement() {
    power_management_animation_refresh_.Stop();
    power_management_action_sink_ = nullptr;
    power_management_action_context_ = nullptr;
    power_management_switch_ = nullptr;
    power_management_timeout_dropdown_ = nullptr;
    power_management_updating_ = false;
    if (active_screen_ == Screen::kPowerManagement) {
        active_screen_ = Screen::kNone;
        root_ = nullptr;
    }
}

bool SystemDetailUi::PowerManagementVisible() const { return active_screen_ == Screen::kPowerManagement; }

void* SystemDetailUi::PowerManagementActionContext() const { return power_management_action_context_; }

void SystemDetailUi::PowerManagementBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->power_management_action_sink_ != nullptr) {
        ui->power_management_action_sink_(
            ui->power_management_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kClosePowerManagement});
    }
}

void SystemDetailUi::PowerManagementSwitchEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->power_management_updating_ || ui->power_management_action_sink_ == nullptr) {
        return;
    }
    lv_obj_t* target = lv_event_get_target_obj(event);
    ui->power_management_animation_refresh_.Start(lv_obj_get_display(target));
    const bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    ui->power_management_action_sink_(
        ui->power_management_action_context_,
        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetAutoSleepEnabled, .value = enabled ? 1U : 0U});
}

void SystemDetailUi::PowerManagementTimeoutEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->power_management_updating_ || ui->power_management_action_sink_ == nullptr) {
        return;
    }
    const uint32_t selected = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (selected < kAutoSleepTimeoutMinutes.size()) {
        ui->power_management_action_sink_(
            ui->power_management_action_context_,
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
    LeaveSystemInformation();
    LeaveAppManagement();
    LeavePowerManagement();
    root_ = root;
    remote_control_action_sink_ = action_sink;
    remote_control_action_context_ = action_context;
    remote_control_model_ = model;
    remote_control_off_confirmation_visible_ = false;
    active_screen_ = Screen::kRemoteControl;
    RenderRemoteControlLocked();
    ESP_LOGI(kTag, "Remote Control visible: enabled=%s state=%u", model.enabled ? "yes" : "no",
             static_cast<unsigned>(model.connection_state));
    return {};
}

void SystemDetailUi::UpdateRemoteControlLocked(const host_ui::RemoteControlModel& model) {
    if (!RemoteControlVisible() || root_ == nullptr) {
        return;
    }
    remote_control_model_ = model;
    if (!model.enabled) {
        remote_control_off_confirmation_visible_ = false;
    }
    RenderRemoteControlLocked();
}

void SystemDetailUi::RenderRemoteControlLocked() {
    if (root_ == nullptr) {
        return;
    }
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    (void)DrawDetailHeader(root_, "Remote Control", "Control service and temporary code", RemoteControlBackEvent, this);

    lv_obj_t* status =
        CreatePanel(root_, Bounds{.x = 40, .y = 126, .width = 640, .height = 128}, 0x111f32U, 0x2e4562U, 22);
    (void)CreatePanel(status, Bounds{.x = 24, .y = 24, .width = 13, .height = 13},
                      RemoteControlStateColor(remote_control_model_.connection_state), 0U, LV_RADIUS_CIRCLE);
    (void)CreateLabel(status, RemoteControlStateText(remote_control_model_.connection_state),
                      BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 54, 16);
    (void)CreatePanel(status, Bounds{.x = 20, .y = 61, .width = 600, .height = 1}, 0x21364eU, 0U, 0);
    (void)CreateLabel(status, "Service", BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 22, 82);
    lv_obj_t* service = CreateLabel(status, DisplayText(remote_control_model_.service.data(), "Not configured"),
                                    BuiltinLatinFont(SystemFontRole::kMedium), 0xf2f7ffU, 260, 82);
    lv_obj_set_width(service, 356);
    lv_obj_set_style_text_align(service, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(service, LV_LABEL_LONG_DOT);

    const bool code_available = remote_control_model_.pairing_code_available;
    const bool code_pending = remote_control_model_.pairing_code_pending;
    const bool pairing_available =
        remote_control_model_.enabled &&
        remote_control_model_.connection_state == host_ui::RemoteControlConnectionState::kConnected;
    const int32_t pairing_height = code_available ? 300 : code_pending ? 264 : 224;
    lv_obj_t* pairing =
        CreatePanel(root_, Bounds{.x = 40, .y = 278, .width = 640, .height = pairing_height}, 0x111f32U, 0x2e4562U, 22);
    if (code_available) {
        lv_obj_t* eyebrow =
            CreateLabel(pairing, "CONNECTION CODE", BuiltinLatinFont(SystemFontRole::kMedium), 0xc5f36dU, 24, 24);
        lv_obj_set_width(eyebrow, 592);
        lv_obj_set_style_text_align(eyebrow, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t* code_panel =
            CreatePanel(pairing, Bounds{.x = 22, .y = 64, .width = 596, .height = 112}, 0x0d1929U, 0xc5f36dU, 16);
        lv_obj_t* code = CreateLabel(code_panel, DisplayText(remote_control_model_.pairing_code.data(), "---- ----"),
                                     BuiltinLatinFont(SystemFontRole::kTitle), 0xc5f36dU, 0, 36);
        lv_obj_set_width(code, 596);
        lv_obj_set_style_text_align(code, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_letter_space(code, 6, 0);
        char expiry[48]{};
        const uint32_t minutes = remote_control_model_.pairing_expires_seconds / 60U;
        const uint32_t seconds = remote_control_model_.pairing_expires_seconds % 60U;
        std::snprintf(expiry, sizeof(expiry), "Expires in %02" PRIu32 ":%02" PRIu32, minutes, seconds);
        lv_obj_t* expiry_label =
            CreateLabel(pairing, expiry, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 198);
        lv_obj_set_width(expiry_label, 592);
        lv_obj_set_style_text_align(expiry_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t* note = CreateLabel(pairing, "Connection codes are single-use and expire after 5 minutes",
                                     BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, 24, 252);
        lv_obj_set_width(note, 592);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        (void)CreateLabel(pairing, "Connect this device", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 22);
        (void)CreateLabel(pairing, "Use a one-time code to add this device.", BuiltinLatinFont(SystemFontRole::kMedium),
                          0x91a4bdU, 24, 61);
        lv_obj_t* pairing_button = CreateActionButton(pairing, Bounds{.x = 22, .y = 104, .width = 596, .height = 70},
                                                      code_pending        ? "Getting connection code..."
                                                      : pairing_available ? "Generate Connection Code"
                                                                          : "Control service unavailable",
                                                      code_pending || !pairing_available ? 0x91a4bdU : 0xc5f36dU,
                                                      code_pending || !pairing_available ? 0x365472U : 0x526b2cU);
        if (code_pending) {
            lv_obj_set_style_bg_color(pairing_button, lv_color_hex(0x24364eU), 0);
            lv_obj_t* spinner = lv_spinner_create(pairing_button);
            lv_obj_set_pos(spinner, 34, 20);
            lv_obj_set_size(spinner, 30, 30);
            lv_spinner_set_anim_params(spinner, 800U, 230U);
            lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
            lv_obj_set_style_arc_color(spinner, lv_color_hex(0x526983U), LV_PART_MAIN);
            lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(spinner, lv_color_hex(0x69a7ffU), LV_PART_INDICATOR);
            lv_obj_remove_flag(pairing_button, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* waiting = CreateLabel(pairing, "Waiting for Control service",
                                            BuiltinLatinFont(SystemFontRole::kMedium), 0x69a7ffU, 24, 190);
            lv_obj_set_width(waiting, 592);
            lv_obj_set_style_text_align(waiting, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t* note = CreateLabel(pairing, "Connection codes are single-use and expire after 5 minutes",
                                         BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, 24, 226);
            lv_obj_set_width(note, 592);
            lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        } else {
            if (pairing_available) {
                lv_obj_add_event_cb(pairing_button, RemoteControlPairingEvent, LV_EVENT_SHORT_CLICKED, this);
            } else {
                lv_obj_remove_flag(pairing_button, LV_OBJ_FLAG_CLICKABLE);
            }
            lv_obj_t* note = CreateLabel(pairing, "Connection codes are single-use and expire after 5 minutes",
                                         BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, 24, 190);
            lv_obj_set_width(note, 592);
            lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    (void)CreatePanel(root_, Bounds{.x = 40, .y = 590, .width = 640, .height = 1}, 0x21364eU, 0U, 0);
    lv_obj_t* toggle =
        CreatePanel(root_, Bounds{.x = 40, .y = 610, .width = 640, .height = 82}, 0x0d1929U, 0x2e4562U, 18);
    lv_obj_add_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toggle, RemoteControlToggleEvent, LV_EVENT_SHORT_CLICKED, this);
    (void)CreateLabel(toggle, LV_SYMBOL_POWER, BuiltinLatinFont(SystemFontRole::kLarge),
                      remote_control_model_.enabled ? 0xff6b74U : 0x69a7ffU, 24, 27);
    (void)CreateLabel(toggle, remote_control_model_.enabled ? "Turn Remote Control Off" : "Turn Remote Control On",
                      BuiltinLatinFont(SystemFontRole::kMedium), 0xf2f7ffU, 74, 16);
    (void)CreateLabel(toggle, remote_control_model_.enabled ? "Requires confirmation" : "Remote access is disabled",
                      BuiltinLatinFont(SystemFontRole::kMedium), 0x748aa5U, 74, 46);
    (void)CreateLabel(toggle, LV_SYMBOL_RIGHT, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 594, 31);

    if (remote_control_off_confirmation_visible_) {
        DrawRemoteControlOffConfirmationLocked();
    }
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

void SystemDetailUi::LeaveRemoteControl() {
    remote_control_action_sink_ = nullptr;
    remote_control_action_context_ = nullptr;
    remote_control_model_ = {};
    remote_control_off_confirmation_visible_ = false;
    if (active_screen_ == Screen::kRemoteControl) {
        active_screen_ = Screen::kNone;
        root_ = nullptr;
    }
}

bool SystemDetailUi::RemoteControlVisible() const { return active_screen_ == Screen::kRemoteControl; }

void* SystemDetailUi::RemoteControlActionContext() const { return remote_control_action_context_; }

void SystemDetailUi::RemoteControlBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->remote_control_action_sink_ != nullptr) {
        ui->remote_control_action_sink_(
            ui->remote_control_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseRemoteControl});
    }
}

void SystemDetailUi::RemoteControlToggleEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->remote_control_action_sink_ == nullptr) {
        return;
    }
    if (ui->remote_control_model_.enabled) {
        ui->remote_control_off_confirmation_visible_ = true;
        ui->QueueRemoteControlRender();
        return;
    }
    ui->remote_control_action_sink_(
        ui->remote_control_action_context_,
        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetRemoteControlEnabled, .value = 1U});
}

void SystemDetailUi::RemoteControlPairingEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->remote_control_action_sink_ == nullptr || !ui->remote_control_model_.enabled ||
        ui->remote_control_model_.connection_state != host_ui::RemoteControlConnectionState::kConnected ||
        ui->remote_control_model_.pairing_code_pending || ui->remote_control_model_.pairing_code_available) {
        return;
    }
    ESP_LOGI(kTag, "Remote Control pairing button short-clicked: action=generate");
    ui->remote_control_action_sink_(
        ui->remote_control_action_context_,
        host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kGenerateRemoteControlPairingCode});
}

void SystemDetailUi::RemoteControlConfirmationCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->remote_control_off_confirmation_visible_ = false;
    ui->QueueRemoteControlRender();
}

void SystemDetailUi::RemoteControlConfirmOffEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->remote_control_action_sink_ == nullptr || !ui->remote_control_model_.enabled) {
        return;
    }
    ui->remote_control_off_confirmation_visible_ = false;
    ui->remote_control_action_sink_(
        ui->remote_control_action_context_,
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
    CreateDismissibleScrim(root_, RemoteControlConfirmationCancelEvent, this);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 390, .width = 664, .height = 302}, 0x101c2cU, 0x653c48U, 24);
    (void)CreateLabel(sheet, "Turn Off Remote Control?", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 20);
    lv_obj_t* detail = CreateLabel(sheet, "Remote access and active connection codes will stop.",
                                   BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 64);
    lv_obj_set_size(detail, 616, 52);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* turn_off = CreateActionButton(sheet, Bounds{.x = 20, .y = 128, .width = 624, .height = 58}, "Turn Off",
                                            0xff6b74U, 0x653c48U);
    lv_obj_add_event_cb(turn_off, RemoteControlConfirmOffEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 208, .width = 624, .height = 58}, "Cancel", 0xf2f7ffU);
    lv_obj_add_event_cb(cancel, RemoteControlConfirmationCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

std::expected<void, host_ui::SystemUiError> SystemDetailUi::ShowAppManagementLocked(
    lv_obj_t* root, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
    void* action_context) {
    if (root == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    LeaveSystemInformation();
    LeaveRemoteControl();
    LeavePowerManagement();
    root_ = root;
    app_management_action_sink_ = action_sink;
    app_management_action_context_ = action_context;
    app_management_model_ = model;
    app_management_selected_index_ = 0U;
    app_management_scroll_y_ = 0;
    app_management_overlay_ = AppOverlay::kNone;
    active_screen_ = Screen::kAppManagement;
    RenderAppManagementLocked(false);
    ESP_LOGI(kTag, "App Management visible: apps=%" PRIu32, model.app_count);
    return {};
}

void SystemDetailUi::LeaveAppManagement() {
    app_management_action_sink_ = nullptr;
    app_management_action_context_ = nullptr;
    app_management_overlay_ = AppOverlay::kNone;
    app_management_model_ = {};
    app_management_scroll_ = nullptr;
    app_management_rows_.fill(nullptr);
    app_management_scroll_y_ = 0;
    if (active_screen_ == Screen::kAppManagement) {
        active_screen_ = Screen::kNone;
        root_ = nullptr;
    }
}

bool SystemDetailUi::AppManagementVisible() const { return active_screen_ == Screen::kAppManagement; }

void* SystemDetailUi::AppManagementActionContext() const { return app_management_action_context_; }

uint32_t SystemDetailUi::FindAppManagementRowIndex(lv_obj_t* row) const {
    for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
        if (app_management_rows_[index] == row) {
            return index;
        }
    }
    return app_management_model_.app_count;
}

void SystemDetailUi::AppManagementRenderAsync(void* context) {
    auto* ui = static_cast<SystemDetailUi*>(context);
    if (ui != nullptr && ui->AppManagementVisible()) {
        ui->RenderAppManagementLocked(true);
    }
}

void SystemDetailUi::QueueAppManagementRender() { (void)lv_async_call(AppManagementRenderAsync, this); }

void SystemDetailUi::AppManagementBackEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->app_management_action_sink_ != nullptr) {
        ui->app_management_action_sink_(
            ui->app_management_action_context_,
            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseAppManagement});
    }
}

void SystemDetailUi::AppManagementRowEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    const uint32_t index = ui->FindAppManagementRowIndex(lv_event_get_current_target_obj(event));
    if (index >= ui->app_management_model_.app_count) {
        return;
    }
    ui->app_management_selected_index_ = index;
    ui->app_management_overlay_ = AppOverlay::kActions;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->app_management_overlay_ = AppOverlay::kNone;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementOpenEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->app_management_action_sink_ == nullptr ||
        ui->app_management_selected_index_ >= ui->app_management_model_.app_count) {
        return;
    }
    ui->app_management_action_sink_(ui->app_management_action_context_,
                                    host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kLaunchInstalledApp,
                                                            .app_index = ui->app_management_selected_index_});
}

void SystemDetailUi::AppManagementInformationEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->app_management_overlay_ = AppOverlay::kInformation;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    ui->app_management_overlay_ = ui->app_management_model_.uninstall_available ? AppOverlay::kUninstallConfirmation
                                                                                : AppOverlay::kUninstallUnavailable;
    ui->QueueAppManagementRender();
}

void SystemDetailUi::AppManagementConfirmUninstallEvent(lv_event_t* event) {
    auto* ui = static_cast<SystemDetailUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->app_management_action_sink_ == nullptr || !ui->app_management_model_.uninstall_available ||
        ui->app_management_selected_index_ >= ui->app_management_model_.app_count) {
        return;
    }
    ui->app_management_action_sink_(ui->app_management_action_context_,
                                    host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kUninstallInstalledApp,
                                                            .app_index = ui->app_management_selected_index_});
}

void SystemDetailUi::DrawAppManagementActionsLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    CreateDismissibleScrim(root_, AppManagementCancelEvent, this);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 330, .width = 664, .height = 362}, 0x101c2cU, 0x42607fU, 24);
    lv_obj_t* title = CreateLabel(sheet, app.display_name, BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 20);
    lv_obj_set_width(title, 616);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* open = CreateActionButton(sheet, Bounds{.x = 20, .y = 72, .width = 624, .height = 58},
                                        app_management_model_.launch_available ? "Open" : "Open unavailable",
                                        app_management_model_.launch_available ? 0xf2f7ffU : 0x708198U);
    if (app_management_model_.launch_available) {
        lv_obj_add_event_cb(open, AppManagementOpenEvent, LV_EVENT_SHORT_CLICKED, this);
    } else {
        lv_obj_remove_flag(open, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* information =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 140, .width = 624, .height = 58}, "App Information", 0xf2f7ffU);
    lv_obj_add_event_cb(information, AppManagementInformationEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* uninstall = CreateActionButton(sheet, Bounds{.x = 20, .y = 208, .width = 624, .height = 58}, "Uninstall",
                                             0xff6b74U, 0x653c48U);
    lv_obj_add_event_cb(uninstall, AppManagementUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 286, .width = 624, .height = 54}, "Cancel", 0xf2f7ffU);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementInformationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    CreateDismissibleScrim(root_, AppManagementCancelEvent, this);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 352, .width = 664, .height = 340}, 0x101c2cU, 0x42607fU, 24);
    lv_obj_t* title = CreateLabel(sheet, app.display_name, BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 20);
    lv_obj_set_width(title, 616);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* details =
        CreatePanel(sheet, Bounds{.x = 20, .y = 66, .width = 624, .height = 174}, 0x111f32U, 0x2e4562U, 18);
    DrawInformationRow(details, 0, "App ID", app.app_id != nullptr ? app.app_id : "Unknown");
    char size[24]{};
    FormatInformationSize(app.bundle_size_kib, size, sizeof(size));
    DrawInformationRow(details, 58, "Bundle Size", size);
    DrawInformationRow(details, 116, "Runtime", "WebAssembly AOT");
    lv_obj_t* done =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 264, .width = 624, .height = 54}, "Done", 0xf2f7ffU);
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallUnavailableLocked() {
    CreateDismissibleScrim(root_, AppManagementCancelEvent, this);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 420, .width = 664, .height = 272}, 0x101c2cU, 0x42607fU, 24);
    (void)CreateLabel(sheet, "Uninstall unavailable", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 20);
    lv_obj_t* detail = CreateLabel(sheet, "Close the running App from the Hall before uninstalling Apps.",
                                   BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 64);
    lv_obj_set_size(detail, 616, 92);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_t* done =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 196, .width = 624, .height = 54}, "Done", 0xf2f7ffU);
    lv_obj_add_event_cb(done, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementUninstallConfirmationLocked() {
    const auto& app = app_management_model_.apps[app_management_selected_index_];
    CreateDismissibleScrim(root_, AppManagementCancelEvent, this);
    lv_obj_t* sheet =
        CreatePanel(root_, Bounds{.x = 28, .y = 390, .width = 664, .height = 302}, 0x101c2cU, 0x653c48U, 24);
    (void)CreateLabel(sheet, "Uninstall App?", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 20);
    lv_obj_t* name = CreateLabel(sheet, app.display_name, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 61);
    lv_obj_set_width(name, 616);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t* uninstall = CreateActionButton(sheet, Bounds{.x = 20, .y = 112, .width = 624, .height = 58}, "Uninstall",
                                             0xff6b74U, 0x653c48U);
    lv_obj_add_event_cb(uninstall, AppManagementConfirmUninstallEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel =
        CreateActionButton(sheet, Bounds{.x = 20, .y = 190, .width = 624, .height = 58}, "Cancel", 0xf2f7ffU);
    lv_obj_add_event_cb(cancel, AppManagementCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void SystemDetailUi::DrawAppManagementRowLocked(uint32_t index) {
    if (app_management_scroll_ == nullptr || index >= app_management_model_.app_count ||
        index >= app_management_rows_.size()) {
        return;
    }
    constexpr std::array<uint32_t, 3U> kAppIconColors{0x2c7867U, 0x305f9eU, 0x76539bU};
    const auto& app = app_management_model_.apps[index];
    const int32_t y = kAppManagementRowTop + static_cast<int32_t>(index) * kAppManagementRowStep;
    lv_obj_t* row =
        CreatePanel(app_management_scroll_, Bounds{.x = 40, .y = y, .width = 640, .height = kAppManagementRowHeight},
                    0x111f32U, 0x2e4562U, 22);
    app_management_rows_[index] = row;
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x091522U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(row, AppManagementRowEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* icon = CreatePanel(row, Bounds{.x = 16, .y = 20, .width = 64, .height = 64},
                                 kAppIconColors[index % kAppIconColors.size()], 0U, 17);
    char initial[2]{'?', '\0'};
    if (app.display_name != nullptr && app.display_name[0] != '\0') {
        initial[0] = app.display_name[0];
    }
    lv_obj_t* initial_label = lv_label_create(icon);
    lv_label_set_text(initial_label, initial);
    lv_obj_set_style_text_font(initial_label, BuiltinLatinFont(SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(initial_label, lv_color_white(), 0);
    lv_obj_center(initial_label);
    lv_obj_t* name = CreateLabel(row, app.display_name != nullptr ? app.display_name : "Unknown App",
                                 BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 98, 20);
    lv_obj_set_width(name, 438);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    char metadata[128]{};
    char size[24]{};
    FormatInformationSize(app.bundle_size_kib, size, sizeof(size));
    std::snprintf(metadata, sizeof(metadata), "%s   %s", app.app_id != nullptr ? app.app_id : "Unknown", size);
    lv_obj_t* metadata_label = CreateLabel(row, metadata, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 98, 59);
    lv_obj_set_width(metadata_label, 438);
    lv_label_set_long_mode(metadata_label, LV_LABEL_LONG_DOT);
    DrawAppMoreIndicator(row);
}

void SystemDetailUi::RefreshAppManagementRowsLocked() {
    if (!AppManagementVisible() || app_management_scroll_ == nullptr || app_management_model_.app_count == 0U) {
        return;
    }
    app_management_scroll_y_ = std::max<int32_t>(0, lv_obj_get_scroll_y(app_management_scroll_));
    const int32_t viewport_bottom = app_management_scroll_y_ + (kScreenHeight - 108);
    uint32_t first =
        app_management_scroll_y_ <= kAppManagementRowTop
            ? 0U
            : static_cast<uint32_t>((app_management_scroll_y_ - kAppManagementRowTop) / kAppManagementRowStep);
    first = first > kAppManagementRowOverscan ? first - kAppManagementRowOverscan : 0U;
    const int32_t last_position = std::max<int32_t>(0, viewport_bottom - kAppManagementRowTop);
    const uint32_t last = std::min<uint32_t>(
        app_management_model_.app_count,
        static_cast<uint32_t>(last_position / kAppManagementRowStep) + 1U + kAppManagementRowOverscan);
    for (uint32_t index = 0U; index < app_management_model_.app_count; ++index) {
        if (app_management_rows_[index] != nullptr && (index < first || index >= last)) {
            lv_obj_delete(app_management_rows_[index]);
            app_management_rows_[index] = nullptr;
        }
    }
    for (uint32_t index = first; index < last; ++index) {
        if (app_management_rows_[index] == nullptr) {
            DrawAppManagementRowLocked(index);
        }
    }
}

void SystemDetailUi::RenderAppManagementLocked(bool clean_root) {
    if (app_management_scroll_ != nullptr) {
        app_management_scroll_y_ = std::max<int32_t>(0, lv_obj_get_scroll_y(app_management_scroll_));
    }
    if (clean_root) {
        lv_obj_clean(root_);
    }
    app_management_scroll_ = nullptr;
    app_management_rows_.fill(nullptr);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x08111fU), 0);
    (void)DrawDetailHeader(root_, "App Management", "Installed applications", AppManagementBackEvent, this);

    const int32_t content_height = std::max<int32_t>(
        kScreenHeight - 108,
        106 + static_cast<int32_t>(std::max<uint32_t>(1U, app_management_model_.app_count)) * 116 + 30);
    lv_obj_t* scroll_content = CreateDetailScrollContent(root_, content_height, DetailScrollEvent, this);
    app_management_scroll_ = scroll_content;
    char count[48]{};
    if (app_management_model_.app_count == 1U) {
        std::snprintf(count, sizeof(count), "1 App");
    } else {
        std::snprintf(count, sizeof(count), "%" PRIu32 " Apps", app_management_model_.app_count);
    }
    (void)CreateLabel(scroll_content, count, BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 42, 14);
    (void)CreateLabel(scroll_content, "Installed on this device", BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU,
                      42, 49);
    char used[24]{};
    char total[24]{};
    char storage[56]{};
    FormatInformationSize(app_management_model_.storage_used_kib, used, sizeof(used));
    FormatInformationSize(app_management_model_.storage_total_kib, total, sizeof(total));
    if (app_management_model_.storage_total_kib != 0U) {
        std::snprintf(storage, sizeof(storage), "%s / %s", used, total);
    } else {
        std::snprintf(storage, sizeof(storage), "%s used", used);
    }
    lv_obj_t* used_label =
        CreateLabel(scroll_content, storage, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 448, 27);
    lv_obj_set_width(used_label, 230);
    lv_obj_set_style_text_align(used_label, LV_TEXT_ALIGN_RIGHT, 0);

    if (app_management_model_.app_count == 0U) {
        lv_obj_t* empty = CreatePanel(scroll_content, Bounds{.x = 40, .y = 90, .width = 640, .height = 104}, 0x111f32U,
                                      0x2e4562U, 22);
        (void)CreateLabel(empty, "No installed Apps", BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 22, 39);
    }
    lv_obj_update_layout(scroll_content);
    if (app_management_scroll_y_ != 0) {
        lv_obj_scroll_to_y(scroll_content, app_management_scroll_y_, LV_ANIM_OFF);
    }
    RefreshAppManagementRowsLocked();
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
    lv_obj_move_foreground(root_);
    RequestDisplayRefresh(lv_obj_get_display(root_));
}

}  // namespace micropixel::platform::lvgl::square_720
