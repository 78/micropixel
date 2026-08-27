#include "platform/lvgl/ui/square_720/wifi_settings_ui.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

namespace micropixel::platform::lvgl::square_720 {
namespace {

constexpr char kTag[] = "micropixel_wifi_ui";
constexpr int32_t kWidth = 720;
constexpr int32_t kHeight = 720;
constexpr uint32_t kThemeAccentColor = 0x287ee8U;

struct WifiBounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

lv_obj_t* CreateWifiLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int32_t x,
                          int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

}  // namespace

struct WifiSettingsUiAccess final {
    static void ResetObjectPointers(WifiSettingsUi& state);
    static void WifiKeyboardTrackPointerEvent(lv_event_t* event);
    static void WifiKeyboardEvent(lv_event_t* event);
    static void DrawWifiKeyboard(WifiSettingsUi& state, lv_obj_t* parent);
    static void DrawWifiPasswordOverlayLocked(WifiSettingsUi& state);
    static void DrawWifiActionSheetLocked(WifiSettingsUi& state);
    static void RenderWifiSettingsLocked(WifiSettingsUi& state);
    static void EmitWifiNetworkAction(WifiSettingsUi& state, host_ui::SystemUiActionType type,
                                      const host_ui::WifiNetworkModel& network);
    static void WifiSettingsRenderAsync(void* context);
    static void QueueWifiSettingsRender(WifiSettingsUi& state);
    static void WifiScrollEvent(lv_event_t* event);
    static void WifiBackEvent(lv_event_t* event);
    static void WifiOpenScanEvent(lv_event_t* event);
    static void WifiSwitchEvent(lv_event_t* event);
    static void WifiSavedNetworkEvent(lv_event_t* event);
    static void WifiAvailableNetworkEvent(lv_event_t* event);
    static void WifiSheetConnectEvent(lv_event_t* event);
    static void WifiSheetForgetEvent(lv_event_t* event);
    static void WifiSheetCancelEvent(lv_event_t* event);
    static void RaiseOverlayLocked(WifiSettingsUi& state);
    static std::expected<void, host_ui::SystemUiError> ShowLocked(
        WifiSettingsUi& state, lv_obj_t* root, lv_display_t* display, const host_ui::WifiSettingsModel& model,
        host_ui::SystemUiActionSink action_sink, void* action_context,
        WifiSettingsUi::RaiseOverlaySink raise_overlay_sink, void* raise_overlay_context);
    static void Update(WifiSettingsUi& state, const host_ui::WifiSettingsModel& model, bool pointer_busy);
    static void Leave(WifiSettingsUi& state);
    static void PointerReleased(WifiSettingsUi& state);
};

constexpr int32_t kWifiContentLeft = 40;
constexpr int32_t kWifiContentWidth = 640;
constexpr int32_t kWifiRowHeight = 76;
constexpr int32_t kWifiRowGap = 10;
constexpr int32_t kWifiSavedStartY = 364;

struct WifiListLayout final {
    int32_t saved_start{};
    int32_t available_heading{};
    int32_t available_start{};
    int32_t content_height{};
};

WifiListLayout GetWifiListLayout(const host_ui::WifiSettingsModel& model, bool scan_view) {
    if (scan_view) {
        const int32_t available_heading = 118;
        const int32_t available_start = available_heading + 36;
        const uint32_t available_rows = model.available_network_count == 0U ? 1U : model.available_network_count;
        const int32_t content_height =
            available_start + static_cast<int32_t>(available_rows) * (kWifiRowHeight + kWifiRowGap) + 30;
        return WifiListLayout{.available_heading = available_heading,
                              .available_start = available_start,
                              .content_height = content_height};
    }
    const uint32_t saved_rows = model.saved_network_count == 0U ? 1U : model.saved_network_count;
    const int32_t content_height =
        kWifiSavedStartY + static_cast<int32_t>(saved_rows) * (kWifiRowHeight + kWifiRowGap) + 30;
    return WifiListLayout{.saved_start = kWifiSavedStartY, .content_height = content_height};
}

void WifiSettingsUiAccess::ResetObjectPointers(WifiSettingsUi& state) {
    state.wifi_scroll_content = nullptr;
    state.wifi_open_scan_row = nullptr;
    state.wifi_password_textarea = nullptr;
    state.wifi_keyboard = nullptr;
    for (auto& row : state.wifi_saved_rows) {
        row = nullptr;
    }
    for (auto& row : state.wifi_available_rows) {
        row = nullptr;
    }
}

lv_obj_t* CreateWifiPanel(lv_obj_t* parent, const WifiBounds& bounds, uint32_t background, uint32_t border,
                          int32_t radius) {
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

void CreateWifiDismissibleScrim(lv_obj_t* parent, lv_event_cb_t dismiss_callback, void* user_data) {
    lv_obj_t* scrim =
        CreateWifiPanel(parent, WifiBounds{.x = 0, .y = 0, .width = kWidth, .height = kHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrim, dismiss_callback, LV_EVENT_SHORT_CLICKED, user_data);
}

const char* WifiBandText(host_ui::WifiBand band) {
    switch (band) {
        case host_ui::WifiBand::k2_4Ghz:
            return "2.4 GHz";
        case host_ui::WifiBand::k5Ghz:
            return "5 GHz";
        case host_ui::WifiBand::kUnknown:
        default:
            return "Wi-Fi";
    }
}

const char* WifiConnectionDetail(const host_ui::WifiSettingsModel& model) {
    if (!model.available) {
        return "Not available";
    }
    if (!model.enabled) {
        return "Off";
    }
    switch (model.connection_state) {
        case host_ui::WifiConnectionState::kConnecting:
            return "Connecting...";
        case host_ui::WifiConnectionState::kConnected:
            return "Connected";
        case host_ui::WifiConnectionState::kAuthenticationFailed:
        case host_ui::WifiConnectionState::kAuthenticationTimedOut:
        case host_ui::WifiConnectionState::kHandshakeTimedOut:
        case host_ui::WifiConnectionState::kNetworkNotFound:
        case host_ui::WifiConnectionState::kFailed:
        case host_ui::WifiConnectionState::kDisconnected:
        default:
            return model.scanning ? "Updating nearby networks" : "Not connected";
    }
}

bool WifiModelShowsConnectedNetwork(const host_ui::WifiSettingsModel& model, const host_ui::WifiNetworkModel& network) {
    for (uint32_t index = 0U; index < model.saved_network_count; ++index) {
        const host_ui::WifiNetworkModel& saved = model.saved_networks[index];
        if (saved.connected && std::strcmp(saved.ssid.data(), network.ssid.data()) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t WifiSignalLevel(int8_t rssi) {
    if (rssi == 0) {
        return 0U;
    }
    if (rssi >= -50) {
        return 4U;
    }
    if (rssi >= -62) {
        return 3U;
    }
    if (rssi >= -74) {
        return 2U;
    }
    return 1U;
}

void DrawWifiSignal(lv_obj_t* parent, int8_t rssi) {
    constexpr int32_t kBarHeights[] = {10, 18, 27, 36};
    const uint32_t level = WifiSignalLevel(rssi);
    for (uint32_t index = 0U; index < 4U; ++index) {
        lv_obj_t* bar = lv_obj_create(parent);
        lv_obj_set_pos(bar, 20 + static_cast<int32_t>(index) * 8, 56 - kBarHeights[index]);
        lv_obj_set_size(bar, 5, kBarHeights[index]);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x69a7ffU), 0);
        lv_obj_set_style_bg_opa(bar, index < level ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
}

void DrawWifiSecurityLock(lv_obj_t* parent) {
    constexpr lv_border_side_t kShackleSides = static_cast<lv_border_side_t>(
        static_cast<uint32_t>(LV_BORDER_SIDE_TOP) | static_cast<uint32_t>(LV_BORDER_SIDE_LEFT) |
        static_cast<uint32_t>(LV_BORDER_SIDE_RIGHT));
    lv_obj_t* shackle = lv_obj_create(parent);
    lv_obj_set_pos(shackle, 582, 19);
    lv_obj_set_size(shackle, 24, 24);
    lv_obj_set_style_pad_all(shackle, 0, 0);
    lv_obj_set_style_radius(shackle, 12, 0);
    lv_obj_set_style_border_width(shackle, 3, 0);
    lv_obj_set_style_border_side(shackle, kShackleSides, 0);
    lv_obj_set_style_border_color(shackle, lv_color_hex(0x91a4bdU), 0);
    lv_obj_set_style_bg_opa(shackle, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(shackle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(shackle, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* body =
        CreateWifiPanel(parent, WifiBounds{.x = 578, .y = 34, .width = 32, .height = 24}, 0x91a4bdU, 0U, 6);
    (void)CreateWifiPanel(body, WifiBounds{.x = 14, .y = 8, .width = 4, .height = 8}, 0x111f32U, 0U, 2);
}

void DrawWifiMoreIndicator(lv_obj_t* parent) {
    for (int32_t index = 0; index < 3; ++index) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_set_pos(dot, 594, 22 + index * 13);
        lv_obj_set_size(dot, 7, 7);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xa9bdd5U), 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t* DrawWifiNetworkRow(lv_obj_t* parent, const host_ui::WifiNetworkModel& network, int32_t y, bool saved) {
    constexpr lv_style_selector_t kPressed = static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    lv_obj_t* row = CreateWifiPanel(
        parent, WifiBounds{.x = kWifiContentLeft, .y = y, .width = kWifiContentWidth, .height = kWifiRowHeight},
        0x111f32U, 0x2e4562U, 20);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x091522U), kPressed);
    DrawWifiSignal(row, network.rssi);
    lv_obj_t* name =
        CreateWifiLabel(row, network.ssid.data(), BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 70, 13);
    lv_obj_set_width(name, network.connected ? 350 : 430);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    char detail[96]{};
    if (saved) {
        std::snprintf(detail, sizeof(detail), "%s   %s   Channel %u", WifiBandText(network.band),
                      network.secured ? "Secured" : "Open", network.channel);
    } else if (network.saved) {
        std::snprintf(detail, sizeof(detail), "%s   Saved   %d dBm", WifiBandText(network.band),
                      static_cast<int>(network.rssi));
    } else {
        std::snprintf(detail, sizeof(detail), "%s   %s   %d dBm", WifiBandText(network.band),
                      network.secured ? "Secured" : "Open", static_cast<int>(network.rssi));
    }
    lv_obj_t* metadata = CreateWifiLabel(row, detail, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 70, 44);
    lv_obj_set_width(metadata, 450);
    lv_label_set_long_mode(metadata, LV_LABEL_LONG_DOT);
    if (network.connected) {
        (void)CreateWifiLabel(row, "Connected", BuiltinLatinFont(SystemFontRole::kMedium), 0x4dd6a4U, 454, 27);
    }
    if (saved) {
        DrawWifiMoreIndicator(row);
    } else if (network.secured) {
        DrawWifiSecurityLock(row);
    }
    return row;
}

lv_obj_t* DrawWifiButton(lv_obj_t* parent, const WifiBounds& bounds, const char* text, uint32_t color,
                         uint32_t border = 0x365472U) {
    constexpr lv_style_selector_t kPressed = static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    lv_obj_t* button = CreateWifiPanel(parent, bounds, 0x16263aU, border, 16);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x0b1726U), kPressed);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, BuiltinLatinFont(SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_center(label);
    return button;
}

constexpr const char* kWifiKeyboardLowerMap[] = {
    "q",  "w",   "e",     "r",  "t",      "y",       "u",     "i", "o", "p", "\n", "a", "s", "d", "f",
    "g",  "h",   "j",     "k",  "l",      "\n",      "Shift", "z", "x", "c", "v",  "b", "n", "m", LV_SYMBOL_BACKSPACE,
    "\n", "123", "Space", "\n", "Cancel", "Connect", "",
};

constexpr const char* kWifiKeyboardUpperMap[] = {
    "Q",  "W",   "E",     "R",  "T",      "Y",       "U",     "I", "O", "P", "\n", "A", "S", "D", "F",
    "G",  "H",   "J",     "K",  "L",      "\n",      "Shift", "Z", "X", "C", "V",  "B", "N", "M", LV_SYMBOL_BACKSPACE,
    "\n", "123", "Space", "\n", "Cancel", "Connect", "",
};

constexpr const char* kWifiKeyboardNumericMap[] = {
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "0",
    "\n",
    "!",
    "@",
    "#",
    "$",
    "%",
    "^",
    "&",
    "*",
    "(",
    ")",
    "\n",
    "-",
    "_",
    "=",
    "+",
    "[",
    "]",
    "{",
    "}",
    LV_SYMBOL_BACKSPACE,
    "\n",
    "ABC",
    "Space",
    "\n",
    "Cancel",
    "Connect",
    "",
};

constexpr std::array<lv_buttonmatrix_ctrl_t, 32U> MakeWifiKeyboardTextControls() {
    std::array<lv_buttonmatrix_ctrl_t, 32U> controls{};
    constexpr auto kReleaseKey = static_cast<lv_buttonmatrix_ctrl_t>(
        LV_BUTTONMATRIX_CTRL_WIDTH_1 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls.fill(kReleaseKey);
    controls[19] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_2 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[27] = controls[19];
    controls[28] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_3 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[29] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_9 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[30] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_5 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[31] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_7 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    return controls;
}

constexpr std::array<lv_buttonmatrix_ctrl_t, 33U> MakeWifiKeyboardNumericControls() {
    std::array<lv_buttonmatrix_ctrl_t, 33U> controls{};
    constexpr auto kReleaseKey = static_cast<lv_buttonmatrix_ctrl_t>(
        LV_BUTTONMATRIX_CTRL_WIDTH_1 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls.fill(kReleaseKey);
    controls[28] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_2 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[29] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_3 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[30] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_9 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[31] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_5 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    controls[32] = static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_WIDTH_7 | LV_BUTTONMATRIX_CTRL_CLICK_TRIG |
                                                       LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    return controls;
}

constexpr auto kWifiKeyboardTextControls = MakeWifiKeyboardTextControls();
constexpr auto kWifiKeyboardNumericControls = MakeWifiKeyboardNumericControls();

uint32_t WifiKeyboardButtonAtPoint(lv_obj_t* keyboard, const lv_point_t& point) {
    auto* matrix = reinterpret_cast<lv_buttonmatrix_t*>(keyboard);
    lv_area_t keyboard_area{};
    lv_obj_get_coords(keyboard, &keyboard_area);
    const int32_t width = lv_obj_get_width(keyboard);
    const int32_t height = lv_obj_get_height(keyboard);
    const int32_t left_padding = lv_obj_get_style_pad_left(keyboard, LV_PART_MAIN);
    const int32_t right_padding = lv_obj_get_style_pad_right(keyboard, LV_PART_MAIN);
    const int32_t top_padding = lv_obj_get_style_pad_top(keyboard, LV_PART_MAIN);
    const int32_t bottom_padding = lv_obj_get_style_pad_bottom(keyboard, LV_PART_MAIN);
    constexpr int32_t kMaxExtra = LV_DPI_DEF / 10;
    const int32_t row_padding = lv_obj_get_style_pad_row(keyboard, LV_PART_MAIN);
    const int32_t column_padding = lv_obj_get_style_pad_column(keyboard, LV_PART_MAIN);
    const int32_t row_extra = std::min((row_padding / 2) + 1 + (row_padding & 1), kMaxExtra);
    const int32_t column_extra = std::min((column_padding / 2) + 1 + (column_padding & 1), kMaxExtra);

    for (uint32_t button = 0U; button < matrix->btn_cnt; ++button) {
        lv_area_t area = matrix->button_areas[button];
        area.x1 += keyboard_area.x1 - (area.x1 <= left_padding ? std::min(left_padding, kMaxExtra) : column_extra);
        area.y1 += keyboard_area.y1 - (area.y1 <= top_padding ? std::min(top_padding, kMaxExtra) : row_extra);
        area.x2 += keyboard_area.x1 +
                   (area.x2 >= width - right_padding - 2 ? std::min(right_padding, kMaxExtra) : column_extra);
        area.y2 += keyboard_area.y1 +
                   (area.y2 >= height - bottom_padding - 2 ? std::min(bottom_padding, kMaxExtra) : row_extra);
        if (point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2) {
            return button;
        }
    }
    return LV_BUTTONMATRIX_BUTTON_NONE;
}

void WifiSettingsUiAccess::WifiKeyboardTrackPointerEvent(lv_event_t* event) {
    lv_obj_t* keyboard = lv_event_get_current_target_obj(event);
    lv_indev_t* indev = lv_event_get_indev(event);
    if (keyboard == nullptr || indev == nullptr) {
        return;
    }
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    lv_buttonmatrix_set_selected_button(keyboard, WifiKeyboardButtonAtPoint(keyboard, point));
}

void WifiSettingsUiAccess::WifiKeyboardEvent(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    lv_obj_t* keyboard = lv_event_get_current_target_obj(event);
    if (state == nullptr || keyboard == nullptr || state->wifi_password_textarea == nullptr) {
        return;
    }
    const uint32_t button = lv_keyboard_get_selected_button(keyboard);
    const char* text = lv_keyboard_get_button_text(keyboard, button);
    if (text == nullptr) {
        return;
    }
    if (std::strcmp(text, "123") == 0) {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    } else if (std::strcmp(text, "ABC") == 0) {
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    } else if (std::strcmp(text, "Shift") == 0) {
        const lv_keyboard_mode_t mode = lv_keyboard_get_mode(keyboard);
        lv_keyboard_set_mode(
            keyboard, mode == LV_KEYBOARD_MODE_TEXT_UPPER ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_TEXT_UPPER);
    } else if (std::strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(state->wifi_password_textarea);
    } else if (std::strcmp(text, "Space") == 0) {
        lv_textarea_add_char(state->wifi_password_textarea, ' ');
    } else if (std::strcmp(text, "Cancel") == 0) {
        lv_indev_t* indev = lv_event_get_indev(event);
        if (indev != nullptr && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            return;
        }
        state->wifi_password_visible = false;
        state->wifi_password_attempt_active = false;
        state->wifi_password_connection_state = host_ui::WifiConnectionState::kDisconnected;
        state->wifi_password_network = {};
        QueueWifiSettingsRender(*state);
    } else if (std::strcmp(text, "Connect") == 0) {
        lv_indev_t* indev = lv_event_get_indev(event);
        if (indev != nullptr && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            return;
        }
        const char* password = lv_textarea_get_text(state->wifi_password_textarea);
        if (password != nullptr && password[0] != '\0' && state->wifi_password_network.ssid[0] != '\0') {
            std::snprintf(state->wifi_password.data(), state->wifi_password.size(), "%s", password);
            state->wifi_password_attempt_active = true;
            state->wifi_password_connection_state = host_ui::WifiConnectionState::kConnecting;
            EmitWifiNetworkAction(*state, host_ui::SystemUiActionType::kConnectNewWifi, state->wifi_password_network);
            QueueWifiSettingsRender(*state);
        }
    } else {
        lv_textarea_add_text(state->wifi_password_textarea, text);
    }
}

void WifiSettingsUiAccess::DrawWifiKeyboard(WifiSettingsUi& state, lv_obj_t* parent) {
    constexpr lv_style_selector_t kPressedItem =
        static_cast<lv_style_selector_t>(LV_PART_ITEMS) | static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    state.wifi_keyboard = lv_keyboard_create(parent);
    lv_obj_remove_event_cb(state.wifi_keyboard, lv_keyboard_def_event_cb);
    lv_keyboard_set_map(state.wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, kWifiKeyboardLowerMap,
                        kWifiKeyboardTextControls.data());
    lv_keyboard_set_map(state.wifi_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, kWifiKeyboardUpperMap,
                        kWifiKeyboardTextControls.data());
    lv_keyboard_set_map(state.wifi_keyboard, LV_KEYBOARD_MODE_SPECIAL, kWifiKeyboardNumericMap,
                        kWifiKeyboardNumericControls.data());
    lv_keyboard_set_mode(state.wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(state.wifi_keyboard, state.wifi_password_textarea);
    lv_keyboard_set_popovers(state.wifi_keyboard, false);
    lv_obj_add_event_cb(state.wifi_keyboard, WifiKeyboardTrackPointerEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(state.wifi_keyboard, WifiKeyboardEvent, LV_EVENT_VALUE_CHANGED, &state);
    lv_obj_align(state.wifi_keyboard, LV_ALIGN_TOP_LEFT, 26, 196);
    lv_obj_set_size(state.wifi_keyboard, 620, 436);
    lv_obj_set_style_pad_all(state.wifi_keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(state.wifi_keyboard, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(state.wifi_keyboard, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(state.wifi_keyboard, 18, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.wifi_keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(state.wifi_keyboard, lv_color_hex(0x0b1625U), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.wifi_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_font(state.wifi_keyboard, BuiltinLatinFont(SystemFontRole::kMedium), LV_PART_ITEMS);
    lv_obj_set_style_text_color(state.wifi_keyboard, lv_color_hex(0xf2f7ffU), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(state.wifi_keyboard, lv_color_hex(0x16263aU), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(state.wifi_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(state.wifi_keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(state.wifi_keyboard, lv_color_hex(0x365472U), LV_PART_ITEMS);
    lv_obj_set_style_radius(state.wifi_keyboard, 14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(state.wifi_keyboard, lv_color_hex(0x2f6da9U), kPressedItem);
    lv_obj_set_style_border_color(state.wifi_keyboard, lv_color_hex(0x69a7ffU), kPressedItem);
}

void WifiSettingsUiAccess::DrawWifiPasswordOverlayLocked(WifiSettingsUi& state) {
    lv_obj_t* scrim =
        CreateWifiPanel(state.root, WifiBounds{.x = 0, .y = 0, .width = kWidth, .height = kHeight}, 0x030913U, 0U, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_80, 0);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* dialog = CreateWifiPanel(state.root, WifiBounds{.x = 24, .y = 52, .width = 672, .height = 648}, 0x101c2cU,
                                       0x42607fU, 24);
    const auto& network = state.wifi_password_network;
    const char* heading = "Join network";
    uint32_t heading_color = 0xf2f7ffU;
    switch (state.wifi_password_connection_state) {
        case host_ui::WifiConnectionState::kConnecting:
            heading = "Connecting...";
            heading_color = 0x69a7ffU;
            break;
        case host_ui::WifiConnectionState::kAuthenticationFailed:
            heading = "Incorrect password. Try again.";
            heading_color = 0xff737dU;
            break;
        case host_ui::WifiConnectionState::kAuthenticationTimedOut:
            heading = "Authentication timed out. Try again.";
            heading_color = 0xffb45eU;
            break;
        case host_ui::WifiConnectionState::kHandshakeTimedOut:
            heading = "Security handshake timed out. Try again.";
            heading_color = 0xffb45eU;
            break;
        case host_ui::WifiConnectionState::kNetworkNotFound:
            heading = "Network not found. Try again.";
            heading_color = 0xffb45eU;
            break;
        case host_ui::WifiConnectionState::kFailed:
            heading = "Connection failed. Try again.";
            heading_color = 0xffb45eU;
            break;
        default:
            break;
    }
    (void)CreateWifiLabel(dialog, heading, BuiltinLatinFont(SystemFontRole::kLarge), heading_color, 26, 22);
    lv_obj_t* ssid =
        CreateWifiLabel(dialog, network.ssid.data(), BuiltinLatinFont(SystemFontRole::kTitle), 0xf2f7ffU, 26, 58);
    lv_obj_set_width(ssid, 620);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);

    state.wifi_password_textarea = lv_textarea_create(dialog);
    lv_obj_set_pos(state.wifi_password_textarea, 26, 108);
    lv_obj_set_size(state.wifi_password_textarea, 620, 64);
    lv_textarea_set_one_line(state.wifi_password_textarea, true);
    lv_textarea_set_password_mode(state.wifi_password_textarea, false);
    lv_textarea_set_max_length(state.wifi_password_textarea, host_ui::kMaxWifiPasswordLength);
    lv_textarea_set_placeholder_text(state.wifi_password_textarea, "Password");
    if (state.wifi_password[0] != '\0') {
        lv_textarea_set_text(state.wifi_password_textarea, state.wifi_password.data());
    }
    lv_obj_set_style_pad_left(state.wifi_password_textarea, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_right(state.wifi_password_textarea, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_top(state.wifi_password_textarea, 15, LV_PART_MAIN);
    lv_obj_set_style_text_font(state.wifi_password_textarea, BuiltinLatinFont(SystemFontRole::kLarge), LV_PART_MAIN);
    lv_obj_set_style_text_color(state.wifi_password_textarea, lv_color_hex(0xf2f7ffU), LV_PART_MAIN);
    lv_obj_set_style_text_color(state.wifi_password_textarea, lv_color_hex(0x6f849fU), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(state.wifi_password_textarea, lv_color_hex(0x08111fU), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state.wifi_password_textarea, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state.wifi_password_textarea, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(state.wifi_password_textarea, lv_color_hex(0x42607fU), LV_PART_MAIN);
    lv_obj_set_style_radius(state.wifi_password_textarea, 14, LV_PART_MAIN);
    lv_obj_remove_flag(state.wifi_password_textarea, LV_OBJ_FLAG_CLICKABLE);
    DrawWifiKeyboard(state, dialog);
    if (state.wifi_password_connection_state == host_ui::WifiConnectionState::kConnecting) {
        lv_buttonmatrix_set_button_ctrl_all(state.wifi_keyboard, LV_BUTTONMATRIX_CTRL_DISABLED);
        const uint32_t cancel_button =
            lv_keyboard_get_mode(state.wifi_keyboard) == LV_KEYBOARD_MODE_SPECIAL ? 31U : 30U;
        lv_buttonmatrix_clear_button_ctrl(state.wifi_keyboard, cancel_button, LV_BUTTONMATRIX_CTRL_DISABLED);
    }
}

void WifiSettingsUiAccess::DrawWifiActionSheetLocked(WifiSettingsUi& state) {
    CreateWifiDismissibleScrim(state.root, WifiSheetCancelEvent, &state);
    lv_obj_t* sheet = CreateWifiPanel(state.root, WifiBounds{.x = 28, .y = 304, .width = 664, .height = 388}, 0x101c2cU,
                                      0x42607fU, 24);
    (void)CreateWifiLabel(sheet, "SAVED NETWORK", BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 24, 20);
    const auto& network = state.wifi_model.saved_networks[state.wifi_selected_index];
    lv_obj_t* ssid =
        CreateWifiLabel(sheet, network.ssid.data(), BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 24, 48);
    lv_obj_set_width(ssid, 616);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_t* connect = DrawWifiButton(sheet, WifiBounds{.x = 20, .y = 102, .width = 624, .height = 72},
                                       network.connected ? "Disconnect" : "Connect", 0xf2f7ffU);
    lv_obj_add_event_cb(connect, WifiSheetConnectEvent, LV_EVENT_SHORT_CLICKED, &state);
    lv_obj_t* forget = DrawWifiButton(sheet, WifiBounds{.x = 20, .y = 188, .width = 624, .height = 72},
                                      "Forget Network", 0xff6b74U, 0x653c48U);
    lv_obj_add_event_cb(forget, WifiSheetForgetEvent, LV_EVENT_SHORT_CLICKED, &state);
    lv_obj_t* cancel =
        DrawWifiButton(sheet, WifiBounds{.x = 20, .y = 286, .width = 624, .height = 72}, "Cancel", 0xf2f7ffU);
    lv_obj_add_event_cb(cancel, WifiSheetCancelEvent, LV_EVENT_SHORT_CLICKED, &state);
}

void WifiSettingsUiAccess::RenderWifiSettingsLocked(WifiSettingsUi& state) {
    lv_obj_clean(state.root);
    ResetObjectPointers(state);
    lv_obj_set_style_bg_color(state.root, lv_color_hex(0x08111fU), 0);

    const WifiListLayout layout = GetWifiListLayout(state.wifi_model, state.wifi_scan_view);
    state.wifi_scroll_offset =
        std::clamp(state.wifi_scroll_offset, int32_t{0}, std::max<int32_t>(0, layout.content_height - kHeight));
    state.wifi_scroll_content = lv_obj_create(state.root);
    lv_obj_set_pos(state.wifi_scroll_content, 0, 0);
    lv_obj_set_size(state.wifi_scroll_content, kWidth, kHeight);
    lv_obj_set_style_pad_all(state.wifi_scroll_content, 0, 0);
    lv_obj_set_style_border_width(state.wifi_scroll_content, 0, 0);
    lv_obj_set_style_bg_opa(state.wifi_scroll_content, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(state.wifi_scroll_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(state.wifi_scroll_content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(state.wifi_scroll_content, WifiScrollEvent, LV_EVENT_SCROLL, &state);
    lv_obj_add_event_cb(state.wifi_scroll_content, WifiScrollEvent, LV_EVENT_SCROLL_END, &state);
    constexpr lv_obj_flag_t kWifiScrollFlags = static_cast<lv_obj_flag_t>(
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLLABLE) | static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_MOMENTUM) |
        static_cast<uint32_t>(LV_OBJ_FLAG_SCROLL_ELASTIC));
    lv_obj_add_flag(state.wifi_scroll_content, kWifiScrollFlags);
    lv_obj_set_style_width(state.wifi_scroll_content, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(state.wifi_scroll_content, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(state.wifi_scroll_content, lv_color_hex(0x42607fU), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(state.wifi_scroll_content, LV_OPA_COVER, LV_PART_SCROLLBAR);

    lv_obj_t* back = CreateWifiPanel(state.wifi_scroll_content, WifiBounds{.x = 40, .y = 32, .width = 56, .height = 56},
                                     0x0d1929U, 0x42607fU, 18);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x081321U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(back, WifiBackEvent, LV_EVENT_SHORT_CLICKED, &state);
    lv_obj_t* back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_icon, BuiltinLatinFont(SystemFontRole::kLarge), 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_center(back_icon);
    if (state.wifi_scan_view) {
        (void)CreateWifiLabel(state.wifi_scroll_content, "Connect to New Wi-Fi",
                              BuiltinLatinFont(SystemFontRole::kTitle), 0xf2f7ffU, 116, 28);
        (void)CreateWifiLabel(state.wifi_scroll_content,
                              state.wifi_model.scanning ? "Scanning nearby networks..." : "Nearby networks",
                              BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 116, 70);
        (void)CreateWifiLabel(state.wifi_scroll_content, "AVAILABLE NETWORKS",
                              BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 42, layout.available_heading);
        if (!state.wifi_model.enabled || state.wifi_model.available_network_count == 0U) {
            lv_obj_t* empty = CreateWifiPanel(state.wifi_scroll_content,
                                              WifiBounds{.x = kWifiContentLeft,
                                                         .y = layout.available_start,
                                                         .width = kWifiContentWidth,
                                                         .height = kWifiRowHeight},
                                              0x111f32U, 0x2e4562U, 20);
            const char* message = !state.wifi_model.enabled   ? "Turn on Wi-Fi to see networks"
                                  : state.wifi_model.scanning ? "Looking for nearby networks..."
                                                              : "No nearby networks found";
            (void)CreateWifiLabel(empty, message, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 22, 27);
        } else {
            for (uint32_t index = 0U; index < state.wifi_model.available_network_count; ++index) {
                state.wifi_available_rows[index] = DrawWifiNetworkRow(
                    state.wifi_scroll_content, state.wifi_model.available_networks[index],
                    layout.available_start + static_cast<int32_t>(index) * (kWifiRowHeight + kWifiRowGap), false);
                lv_obj_add_event_cb(state.wifi_available_rows[index], WifiAvailableNetworkEvent, LV_EVENT_SHORT_CLICKED,
                                    &state);
            }
        }
    } else {
        (void)CreateWifiLabel(state.wifi_scroll_content, "Wi-Fi", BuiltinLatinFont(SystemFontRole::kTitle), 0xf2f7ffU,
                              116, 28);
        (void)CreateWifiLabel(state.wifi_scroll_content, "Manage connections",
                              BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 116, 70);

        lv_obj_t* toggle =
            CreateWifiPanel(state.wifi_scroll_content, WifiBounds{.x = 40, .y = 116, .width = 640, .height = 86},
                            0x111f32U, 0x365472U, 20);
        lv_obj_add_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(toggle, lv_color_hex(0x0a1726U), static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
        lv_obj_add_event_cb(toggle, WifiSwitchEvent, LV_EVENT_SHORT_CLICKED, &state);
        (void)CreateWifiLabel(toggle, "Wi-Fi", BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 20, 14);
        (void)CreateWifiLabel(toggle, WifiConnectionDetail(state.wifi_model), BuiltinLatinFont(SystemFontRole::kMedium),
                              0x91a4bdU, 20, 48);
        lv_obj_t* switch_track = CreateWifiPanel(toggle, WifiBounds{.x = 532, .y = 19, .width = 84, .height = 48},
                                                 state.wifi_model.enabled ? kThemeAccentColor : 0x31506bU, 0U, 24);
        (void)CreateWifiPanel(switch_track,
                              WifiBounds{.x = state.wifi_model.enabled ? 40 : 4, .y = 4, .width = 40, .height = 40},
                              0xf7fbffU, 0U, 20);

        if (state.wifi_model.enabled) {
            state.wifi_open_scan_row =
                CreateWifiPanel(state.wifi_scroll_content,
                                WifiBounds{.x = kWifiContentLeft, .y = 226, .width = kWifiContentWidth, .height = 76},
                                0x111f32U, 0x2e4562U, 20);
            lv_obj_add_flag(state.wifi_open_scan_row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(state.wifi_open_scan_row, lv_color_hex(0x091522U),
                                      static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
            lv_obj_add_event_cb(state.wifi_open_scan_row, WifiOpenScanEvent, LV_EVENT_SHORT_CLICKED, &state);
            (void)CreateWifiLabel(state.wifi_open_scan_row, "Connect to New Wi-Fi",
                                  BuiltinLatinFont(SystemFontRole::kLarge), 0xf2f7ffU, 20, 24);
            (void)CreateWifiLabel(state.wifi_open_scan_row, LV_SYMBOL_RIGHT, BuiltinLatinFont(SystemFontRole::kLarge),
                                  0x91a4bdU, 592, 24);
        }

        (void)CreateWifiLabel(state.wifi_scroll_content, "SAVED NETWORKS", BuiltinLatinFont(SystemFontRole::kMedium),
                              0x91a4bdU, 42, 326);
        if (state.wifi_model.saved_network_count == 0U) {
            lv_obj_t* empty = CreateWifiPanel(state.wifi_scroll_content,
                                              WifiBounds{.x = kWifiContentLeft,
                                                         .y = layout.saved_start,
                                                         .width = kWifiContentWidth,
                                                         .height = kWifiRowHeight},
                                              0x111f32U, 0x2e4562U, 20);
            (void)CreateWifiLabel(empty, "No saved networks", BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 22,
                                  27);
        } else {
            for (uint32_t index = 0U; index < state.wifi_model.saved_network_count; ++index) {
                state.wifi_saved_rows[index] = DrawWifiNetworkRow(
                    state.wifi_scroll_content, state.wifi_model.saved_networks[index],
                    layout.saved_start + static_cast<int32_t>(index) * (kWifiRowHeight + kWifiRowGap), true);
                lv_obj_add_event_cb(state.wifi_saved_rows[index], WifiSavedNetworkEvent, LV_EVENT_SHORT_CLICKED,
                                    &state);
            }
        }
    }

    lv_obj_t* bottom_spacer = lv_obj_create(state.wifi_scroll_content);
    lv_obj_set_pos(bottom_spacer, 0, layout.content_height - 1);
    lv_obj_set_size(bottom_spacer, 1, 1);
    lv_obj_set_style_border_width(bottom_spacer, 0, 0);
    lv_obj_set_style_bg_opa(bottom_spacer, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bottom_spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(state.wifi_scroll_content);
    lv_obj_scroll_to_y(state.wifi_scroll_content, state.wifi_scroll_offset, LV_ANIM_OFF);
    if (state.wifi_action_sheet_visible) {
        DrawWifiActionSheetLocked(state);
    } else if (state.wifi_password_visible) {
        DrawWifiPasswordOverlayLocked(state);
    }
    lv_obj_move_foreground(state.root);
    RaiseOverlayLocked(state);
    RequestDisplayRefresh(state.display);
}

void WifiSettingsUiAccess::EmitWifiNetworkAction(WifiSettingsUi& state, host_ui::SystemUiActionType type,
                                                 const host_ui::WifiNetworkModel& network) {
    if (state.wifi_action_sink == nullptr) {
        return;
    }
    host_ui::SystemUiAction action{.type = type};
    action.text = network.ssid;
    if (type == host_ui::SystemUiActionType::kConnectNewWifi) {
        action.secret = state.wifi_password;
    }
    state.wifi_action_sink(state.wifi_action_context, action);
}

void WifiSettingsUiAccess::WifiSettingsRenderAsync(void* context) {
    auto* state = static_cast<WifiSettingsUi*>(context);
    if (state != nullptr && state->wifi_action_sink != nullptr) {
        RenderWifiSettingsLocked(*state);
    }
}

void WifiSettingsUiAccess::QueueWifiSettingsRender(WifiSettingsUi& state) {
    (void)lv_async_call(WifiSettingsRenderAsync, &state);
}

void WifiSettingsUiAccess::WifiScrollEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    lv_obj_t* scroll_content = lv_event_get_current_target_obj(event);
    if (state == nullptr || scroll_content == nullptr) {
        return;
    }
    bool render_after_scroll = false;
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
        portENTER_CRITICAL(&state->render_lock);
        state->wifi_scroll_gesture_active = false;
        if (state->wifi_render_pending) {
            state->wifi_render_pending = false;
            render_after_scroll = true;
        }
        portEXIT_CRITICAL(&state->render_lock);
    } else {
        lv_indev_t* indev = lv_indev_active();
        if (indev != nullptr && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            portENTER_CRITICAL(&state->render_lock);
            state->wifi_user_scrolled = true;
            state->wifi_scroll_gesture_active = true;
            portEXIT_CRITICAL(&state->render_lock);
        }
    }
    if (state->wifi_user_scrolled) {
        state->wifi_scroll_offset = std::max<int32_t>(0, lv_obj_get_scroll_y(scroll_content));
    }
    if (render_after_scroll) {
        QueueWifiSettingsRender(*state);
    }
    if (state->display != nullptr) {
        RequestDisplayRefresh(state->display);
    }
}

uint32_t FindWifiRowIndex(lv_obj_t* target, lv_obj_t* const* rows, uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        if (rows[index] == target) {
            return index;
        }
    }
    return count;
}

void WifiSettingsUiAccess::WifiBackEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state != nullptr && state->wifi_action_sink != nullptr) {
        if (state->wifi_scan_view) {
            state->wifi_scan_view = false;
            state->wifi_scroll_offset = 0;
            state->wifi_user_scrolled = false;
            state->wifi_password_visible = false;
            state->wifi_password_attempt_active = false;
            state->wifi_action_sink(
                state->wifi_action_context,
                host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseWifiNetworkScan});
            QueueWifiSettingsRender(*state);
            return;
        }
        state->wifi_action_sink(state->wifi_action_context,
                                host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseWifiSettings});
    }
}

void WifiSettingsUiAccess::WifiOpenScanEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state == nullptr || state->wifi_action_sink == nullptr || !state->wifi_model.enabled) {
        return;
    }
    state->wifi_scan_view = true;
    state->wifi_scroll_offset = 0;
    state->wifi_user_scrolled = false;
    state->wifi_action_sink(state->wifi_action_context,
                            host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kOpenWifiNetworkScan});
    QueueWifiSettingsRender(*state);
}

void WifiSettingsUiAccess::WifiSwitchEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state != nullptr && state->wifi_action_sink != nullptr) {
        state->wifi_action_sink(state->wifi_action_context,
                                host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSetWifiEnabled,
                                                        .value = state->wifi_model.enabled ? 0U : 1U});
    }
}

void WifiSettingsUiAccess::WifiSavedNetworkEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state == nullptr || state->wifi_action_sink == nullptr) {
        return;
    }
    const uint32_t index = FindWifiRowIndex(lv_event_get_current_target_obj(event), state->wifi_saved_rows,
                                            state->wifi_model.saved_network_count);
    if (index >= state->wifi_model.saved_network_count) {
        return;
    }
    state->wifi_selected_index = index;
    state->wifi_action_sheet_visible = true;
    DrawWifiActionSheetLocked(*state);
    RaiseOverlayLocked(*state);
}

void WifiSettingsUiAccess::WifiAvailableNetworkEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state == nullptr || state->wifi_action_sink == nullptr) {
        return;
    }
    const uint32_t index = FindWifiRowIndex(lv_event_get_current_target_obj(event), state->wifi_available_rows,
                                            state->wifi_model.available_network_count);
    if (index >= state->wifi_model.available_network_count) {
        return;
    }
    state->wifi_selected_index = index;
    const auto& network = state->wifi_model.available_networks[index];
    if (network.connected) {
        return;
    }
    if (network.saved) {
        EmitWifiNetworkAction(*state, host_ui::SystemUiActionType::kConnectSavedWifi, network);
        return;
    }
    state->wifi_password.fill('\0');
    if (!network.secured) {
        EmitWifiNetworkAction(*state, host_ui::SystemUiActionType::kConnectNewWifi, network);
        return;
    }
    state->wifi_password_network = network;
    state->wifi_password_connection_state = host_ui::WifiConnectionState::kDisconnected;
    state->wifi_password_attempt_active = false;
    state->wifi_password_visible = true;
    DrawWifiPasswordOverlayLocked(*state);
    RaiseOverlayLocked(*state);
}

void WifiSettingsUiAccess::WifiSheetConnectEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state == nullptr || state->wifi_selected_index >= state->wifi_model.saved_network_count) {
        return;
    }
    const auto& network = state->wifi_model.saved_networks[state->wifi_selected_index];
    EmitWifiNetworkAction(*state,
                          network.connected ? host_ui::SystemUiActionType::kDisconnectWifi
                                            : host_ui::SystemUiActionType::kConnectSavedWifi,
                          network);
    state->wifi_action_sheet_visible = false;
    QueueWifiSettingsRender(*state);
}

void WifiSettingsUiAccess::WifiSheetForgetEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state == nullptr || state->wifi_selected_index >= state->wifi_model.saved_network_count) {
        return;
    }
    EmitWifiNetworkAction(*state, host_ui::SystemUiActionType::kForgetWifi,
                          state->wifi_model.saved_networks[state->wifi_selected_index]);
    state->wifi_action_sheet_visible = false;
    QueueWifiSettingsRender(*state);
}

void WifiSettingsUiAccess::WifiSheetCancelEvent(lv_event_t* event) {
    auto* state = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (state == nullptr) {
        return;
    }
    state->wifi_action_sheet_visible = false;
    state->wifi_password_visible = false;
    state->wifi_password_attempt_active = false;
    state->wifi_password_connection_state = host_ui::WifiConnectionState::kDisconnected;
    state->wifi_password_network = {};
    QueueWifiSettingsRender(*state);
}

void WifiSettingsUiAccess::RaiseOverlayLocked(WifiSettingsUi& state) {
    if (state.raise_overlay_sink != nullptr) {
        state.raise_overlay_sink(state.raise_overlay_context);
    }
}

std::expected<void, host_ui::SystemUiError> WifiSettingsUiAccess::ShowLocked(
    WifiSettingsUi& state, lv_obj_t* root, lv_display_t* display, const host_ui::WifiSettingsModel& model,
    host_ui::SystemUiActionSink action_sink, void* action_context, WifiSettingsUi::RaiseOverlaySink raise_overlay_sink,
    void* raise_overlay_context) {
    if (root == nullptr || display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    state.root = root;
    state.display = display;
    state.wifi_action_sink = action_sink;
    state.wifi_action_context = action_context;
    state.raise_overlay_sink = raise_overlay_sink;
    state.raise_overlay_context = raise_overlay_context;
    state.wifi_model = model;
    state.wifi_scroll_offset = 0;
    state.wifi_user_scrolled = false;
    state.wifi_scroll_gesture_active = false;
    state.wifi_render_pending = false;
    state.wifi_action_sheet_visible = false;
    state.wifi_password_visible = false;
    state.wifi_password_attempt_active = false;
    state.wifi_password_connection_state = host_ui::WifiConnectionState::kDisconnected;
    state.wifi_password_network = {};
    state.wifi_password.fill('\0');
    state.wifi_scan_view = false;
    state.visible = true;
    RenderWifiSettingsLocked(state);
    ESP_LOGI(kTag, "Wi-Fi settings visible: enabled=%s saved=%" PRIu32 " available=%" PRIu32,
             model.enabled ? "yes" : "no", model.saved_network_count, model.available_network_count);
    return {};
}

void WifiSettingsUiAccess::Update(WifiSettingsUi& state, const host_ui::WifiSettingsModel& model, bool pointer_busy) {
    state.wifi_model = model;
    if (!state.visible || state.display == nullptr || state.wifi_action_sheet_visible) {
        return;
    }
    if (state.wifi_password_visible) {
        if (state.wifi_password_attempt_active && WifiModelShowsConnectedNetwork(model, state.wifi_password_network)) {
            state.wifi_password_visible = false;
            state.wifi_password_attempt_active = false;
            state.wifi_password_connection_state = host_ui::WifiConnectionState::kDisconnected;
            state.wifi_password_network = {};
            state.wifi_password.fill('\0');
        } else if (!state.wifi_password_attempt_active) {
            return;
        } else {
            host_ui::WifiConnectionState next_dialog_state = state.wifi_password_connection_state;
            switch (model.connection_state) {
                case host_ui::WifiConnectionState::kConnecting:
                case host_ui::WifiConnectionState::kAuthenticationFailed:
                case host_ui::WifiConnectionState::kAuthenticationTimedOut:
                case host_ui::WifiConnectionState::kHandshakeTimedOut:
                case host_ui::WifiConnectionState::kNetworkNotFound:
                case host_ui::WifiConnectionState::kFailed:
                    next_dialog_state = model.connection_state;
                    break;
                case host_ui::WifiConnectionState::kDisconnected:
                case host_ui::WifiConnectionState::kConnected:
                default:
                    break;
            }
            if (next_dialog_state == state.wifi_password_connection_state) {
                return;
            }
            state.wifi_password_connection_state = next_dialog_state;
        }
    }
    bool defer_render = false;
    portENTER_CRITICAL(&state.render_lock);
    if (pointer_busy || state.wifi_scroll_gesture_active) {
        state.wifi_render_pending = true;
        defer_render = true;
    }
    portEXIT_CRITICAL(&state.render_lock);
    if (defer_render || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    RenderWifiSettingsLocked(state);
    portENTER_CRITICAL(&state.render_lock);
    state.wifi_render_pending = false;
    portEXIT_CRITICAL(&state.render_lock);
    esp_lv_adapter_unlock();
}

void WifiSettingsUiAccess::Leave(WifiSettingsUi& state) {
    state.wifi_action_sink = nullptr;
    state.wifi_action_context = nullptr;
    state.raise_overlay_sink = nullptr;
    state.raise_overlay_context = nullptr;
    state.wifi_action_sheet_visible = false;
    state.wifi_password_visible = false;
    state.wifi_password_attempt_active = false;
    state.wifi_password_connection_state = host_ui::WifiConnectionState::kDisconnected;
    state.wifi_password_network = {};
    state.wifi_scan_view = false;
    portENTER_CRITICAL(&state.render_lock);
    state.wifi_render_pending = false;
    state.wifi_user_scrolled = false;
    state.wifi_scroll_gesture_active = false;
    portEXIT_CRITICAL(&state.render_lock);
    state.visible = false;
    ResetObjectPointers(state);
    state.root = nullptr;
    state.display = nullptr;
}

void WifiSettingsUiAccess::PointerReleased(WifiSettingsUi& state) {
    bool render = false;
    portENTER_CRITICAL(&state.render_lock);
    if (state.visible && state.wifi_render_pending && !state.wifi_scroll_gesture_active) {
        state.wifi_render_pending = false;
        render = true;
    }
    portEXIT_CRITICAL(&state.render_lock);
    if (render) {
        QueueWifiSettingsRender(state);
    }
}

std::expected<void, host_ui::SystemUiError> WifiSettingsUi::ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                       const host_ui::WifiSettingsModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context,
                                                                       RaiseOverlaySink raise_overlay_sink,
                                                                       void* raise_overlay_context) {
    return WifiSettingsUiAccess::ShowLocked(*this, root, display, model, action_sink, action_context,
                                            raise_overlay_sink, raise_overlay_context);
}

void WifiSettingsUi::Update(const host_ui::WifiSettingsModel& model, bool pointer_busy) {
    WifiSettingsUiAccess::Update(*this, model, pointer_busy);
}

void WifiSettingsUi::Leave() { WifiSettingsUiAccess::Leave(*this); }

void WifiSettingsUi::PointerReleased() { WifiSettingsUiAccess::PointerReleased(*this); }

bool WifiSettingsUi::Visible() const { return visible; }

void* WifiSettingsUi::ActionContext() const { return wifi_action_context; }

}  // namespace micropixel::platform::lvgl::square_720
