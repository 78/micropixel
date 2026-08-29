#pragma once

#include <array>
#include <cinttypes>
#include <cstdio>

#include "host/ui/lvgl/square_common/system_page_layout.hpp"
#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common::system_detail_internal {

inline constexpr char kTag[] = "system_details";
inline constexpr std::array<uint8_t, 4U> kAutoSleepTimeoutMinutes{1U, 5U, 10U, 30U};

inline const char* DisplayText(const char* text, const char* fallback) {
    return text != nullptr && text[0] != '\0' ? text : fallback;
}

inline bool FirmwareUpdateInProgress(host_ui::FirmwareUpdateState state) {
    return state == host_ui::FirmwareUpdateState::kDownloading || state == host_ui::FirmwareUpdateState::kVerifying ||
           state == host_ui::FirmwareUpdateState::kInstalling;
}

inline const char* FirmwareUpdateStageText(host_ui::FirmwareUpdateState state) {
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

inline lv_obj_t* Label(lv_obj_t* parent, const char* text, platform::lvgl::SystemFontRole role, uint32_t color) {
    return CreateSystemLabel(parent, text, role, color);
}

inline lv_obj_t* Panel(const SystemPageLayout& layout, lv_obj_t* parent, int32_t gap = -1) {
    return CreateSystemPanel(parent, layout, gap);
}

inline lv_obj_t* Button(const SystemPageLayout& layout, lv_obj_t* parent, const char* text,
                        uint32_t color = theme::kPrimaryText) {
    return CreateSystemActionButton(parent, layout, text, color);
}

inline void Header(const SystemPageLayout& layout, lv_obj_t* root, const char* title, const char* subtitle,
                   lv_event_cb_t back_event, void* context) {
    (void)CreateSystemHeader(root, layout, title, subtitle, back_event, context);
}

inline lv_obj_t* Scroll(const SystemPageLayout& layout, lv_obj_t* root, lv_event_cb_t event, void* context) {
    return CreateSystemScrollColumn(root, layout, event, context);
}

inline void SectionLabel(lv_obj_t* parent, const char* text) {
    (void)Label(parent, text, platform::lvgl::SystemFontRole::kSmall, theme::kMutedText);
}

inline void InformationRow(const SystemPageLayout& layout, lv_obj_t* parent, const char* name, const char* value) {
    (void)CreateSystemInformationRow(parent, layout, name, value);
}

inline void FormatSize(uint32_t kib, char* output, size_t capacity) {
    if (kib >= 1024U) {
        const uint32_t tenths = static_cast<uint32_t>((static_cast<uint64_t>(kib) * 10U + 512U) / 1024U);
        std::snprintf(output, capacity, "%" PRIu32 ".%" PRIu32 " MB", tenths / 10U, tenths % 10U);
    } else {
        std::snprintf(output, capacity, "%" PRIu32 " KB", kib);
    }
}

inline const char* RemoteState(host_ui::RemoteControlConnectionState state) {
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

inline uint32_t RemoteStateColor(host_ui::RemoteControlConnectionState state) {
    switch (state) {
        case host_ui::RemoteControlConnectionState::kConnected:
            return theme::kPositive;
        case host_ui::RemoteControlConnectionState::kWaitingForNetwork:
        case host_ui::RemoteControlConnectionState::kConnecting:
            return theme::kAccent;
        case host_ui::RemoteControlConnectionState::kBackoff:
            return theme::kWarning;
        case host_ui::RemoteControlConnectionState::kAuthenticationError:
            return theme::kDanger;
        case host_ui::RemoteControlConnectionState::kDisabled:
            return theme::kMutedText;
    }
    return theme::kMutedText;
}

inline lv_obj_t* CreateOverlay(lv_obj_t* root, lv_event_cb_t cancel_event, void* context) {
    lv_obj_t* overlay = lv_obj_create(root);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(theme::kModalScrim), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, cancel_event, LV_EVENT_SHORT_CLICKED, context);
    return overlay;
}

inline lv_obj_t* CreateActionSheet(const SystemPageLayout& layout, lv_obj_t* root, lv_event_cb_t cancel_event,
                                   void* context, uint32_t border_color = theme::kStrongBorder,
                                   lv_obj_t** overlay_out = nullptr) {
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

}  // namespace micropixel::host_ui::lvgl::square_common::system_detail_internal
