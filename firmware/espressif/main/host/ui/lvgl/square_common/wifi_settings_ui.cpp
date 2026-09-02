#include "host/ui/lvgl/square_common/wifi_settings_ui.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "host/ui/lvgl/square_common/default_keyboard.hpp"
#include "host/ui/lvgl/square_common/symbols.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

constexpr char kTag[] = "wifi_settings_ui";

const char* SignalText(int8_t rssi) {
    if (rssi >= -55) {
        return "Excellent";
    }
    if (rssi >= -67) {
        return "Good";
    }
    if (rssi >= -75) {
        return "Fair";
    }
    return "Weak";
}

const char* ConnectionText(const host_ui::WifiNetworkModel& network) {
    if (network.connected) {
        return "Connected";
    }
    if (network.saved) {
        return "Saved";
    }
    return SignalText(network.rssi);
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

lv_obj_t* CreateWifiSignal(lv_obj_t* parent, const SystemPageLayout& layout, int8_t rssi, uint32_t color) {
    lv_obj_t* signal = lv_obj_create(parent);
    StyleTransparentContainer(signal);
    const int32_t width = layout.control_height;
    const int32_t height = std::max<int32_t>(18, layout.control_height * 3 / 5);
    lv_obj_set_size(signal, width, height);
    lv_obj_set_flex_flow(signal, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(signal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(signal, std::max<int32_t>(2, width / 16), 0);
    const uint32_t level = WifiSignalLevel(rssi);
    for (uint32_t index = 0U; index < 4U; ++index) {
        lv_obj_t* bar = lv_obj_create(signal);
        const int32_t bar_height = height * static_cast<int32_t>(index + 1U) / 4;
        lv_obj_set_size(bar, std::max<int32_t>(3, width / 10), bar_height);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_radius(bar, std::max<int32_t>(2, width / 20), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(bar, index < level ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }
    return signal;
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

lv_obj_t* CreateOverlay(lv_obj_t* root) {
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
    return overlay;
}

}  // namespace

std::expected<void, host_ui::SystemUiError> WifiSettingsUi::ShowLocked(
    lv_obj_t* root, lv_display_t* display, const SystemPageLayout& layout, const host_ui::WifiSettingsModel& model,
    host_ui::SystemUiActionSink action_sink, void* action_context, RaiseOverlaySink raise_overlay_sink,
    void* raise_overlay_context) {
    if (root == nullptr || display == nullptr || layout.width <= 0 || layout.height <= layout.header_height) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    root_ = root;
    display_ = display;
    layout_ = &layout;
    model_ = model;
    action_sink_ = action_sink;
    action_context_ = action_context;
    raise_overlay_sink_ = raise_overlay_sink;
    raise_overlay_context_ = raise_overlay_context;
    scan_view_ = false;
    action_sheet_visible_ = false;
    password_visible_ = false;
    password_attempt_active_ = false;
    password_length_invalid_ = false;
    password_connection_state_ = host_ui::WifiConnectionState::kDisconnected;
    password_network_ = {};
    password_.fill('\0');
    scroll_offset_ = 0;
    user_scrolled_ = false;
    scroll_gesture_active_ = false;
    render_pending_ = false;
    visible_ = true;
    RenderLocked();
    ESP_LOGI(kTag, "Wi-Fi settings visible: enabled=%s saved=%" PRIu32 " available=%" PRIu32,
             model.enabled ? "yes" : "no", model.saved_network_count, model.available_network_count);
    return {};
}

void WifiSettingsUi::RenderLocked() {
    if (!visible_ || root_ == nullptr || display_ == nullptr || layout_ == nullptr) {
        return;
    }
    const SystemPageLayout& layout = *layout_;
    password_textarea_ = nullptr;
    keyboard_ = nullptr;
    bindings_ = {};
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(theme::kMenuBackground), 0);
    (void)CreateSystemHeader(root_, layout, scan_view_ ? "Available Networks" : "Wi-Fi",
                             scan_view_ ? "Choose a network" : "Wireless network settings", BackEvent, this);
    lv_obj_t* content = CreateSystemScrollColumn(root_, layout, ScrollEvent, this);
    scroll_content_ = content;

    if (!scan_view_) {
        lv_obj_t* wifi_panel = CreateSystemPanel(content, layout);
        lv_obj_set_flex_flow(wifi_panel, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wifi_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(wifi_panel, layout.panel_gap, 0);
        lv_obj_t* wifi_text = CreateSystemColumn(wifi_panel, 2);
        lv_obj_set_width(wifi_text, 0);
        lv_obj_set_flex_grow(wifi_text, 1);
        (void)CreateSystemLabel(wifi_text, "Wi-Fi", layout.heading_font, theme::kPrimaryText);
        const char* status = !model_.available  ? "Not available"
                             : !model_.enabled  ? "Off"
                             : model_.connected ? "Connected"
                                                : "Not connected";
        (void)CreateSystemLabel(wifi_text, status, layout.detail_font, theme::kSecondaryText);
        lv_obj_t* toggle = lv_switch_create(wifi_panel);
        lv_obj_set_size(toggle, layout.control_height + 12, layout.control_height * 3 / 5);
        if (model_.enabled) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        }
        if (!model_.available) {
            lv_obj_add_state(toggle, LV_STATE_DISABLED);
        }
        lv_obj_add_event_cb(toggle, SwitchEvent, LV_EVENT_VALUE_CHANGED, this);
    }

    if (!model_.enabled) {
        lv_obj_t* hint = CreateSystemPanel(content, layout);
        (void)CreateSystemLabel(
            hint,
            layout.width <= 320 ? "Enable Wi-Fi to view networks." : "Turn on Wi-Fi to view and connect to networks.",
            layout.body_font, theme::kSecondaryText);
    } else if (scan_view_) {
        (void)CreateSystemLabel(content, model_.scanning ? "SCANNING..." : "AVAILABLE NETWORKS", layout.detail_font,
                                theme::kSecondaryText);
        if (model_.available_network_count == 0U) {
            lv_obj_t* empty = CreateSystemPanel(content, layout);
            (void)CreateSystemLabel(empty, model_.scanning ? "Looking for networks..." : "No networks found",
                                    layout.body_font, theme::kSecondaryText);
        }
        for (uint32_t index = 0U; index < model_.available_network_count && index < host_ui::kMaxVisibleWifiNetworks;
             ++index) {
            NetworkBinding& binding = bindings_[host_ui::kMaxSavedWifiNetworks + index];
            binding = {.ui = this, .index = index, .saved = false};
            DrawNetworkRow(content, model_.available_networks[index], binding);
        }
    } else {
        lv_obj_t* scan = CreateSystemActionButton(content, layout, "Connect to New Wi-Fi", theme::kAccent);
        lv_obj_add_event_cb(scan, OpenScanEvent, LV_EVENT_SHORT_CLICKED, this);
        (void)CreateSystemLabel(content, "SAVED NETWORKS", layout.detail_font, theme::kSecondaryText);
        if (model_.saved_network_count == 0U) {
            lv_obj_t* empty = CreateSystemPanel(content, layout);
            (void)CreateSystemLabel(empty, "No saved networks", layout.body_font, theme::kSecondaryText);
        }
        for (uint32_t index = 0U; index < model_.saved_network_count && index < host_ui::kMaxSavedWifiNetworks;
             ++index) {
            NetworkBinding& binding = bindings_[index];
            binding = {.ui = this, .index = index, .saved = true};
            DrawNetworkRow(content, model_.saved_networks[index], binding);
        }
    }

    if (action_sheet_visible_) {
        DrawActionSheetLocked();
    } else if (password_visible_) {
        DrawPasswordLocked();
    }
    lv_obj_update_layout(content);
    lv_obj_scroll_to_y(content, scroll_offset_, LV_ANIM_OFF);
    lv_obj_move_foreground(root_);
    if (raise_overlay_sink_ != nullptr) {
        raise_overlay_sink_(raise_overlay_context_);
    }
    platform::lvgl::RequestDisplayRefresh(display_);
}

void WifiSettingsUi::DrawNetworkRow(lv_obj_t* parent, const host_ui::WifiNetworkModel& network,
                                    NetworkBinding& binding) {
    const SystemPageLayout& layout = *layout_;
    lv_obj_t* row = CreateSystemButtonPanel(parent, layout);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, layout.panel_gap, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(theme::kPressedBackground),
                              static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_add_event_cb(row, NetworkEvent, LV_EVENT_SHORT_CLICKED, &binding);
    (void)CreateWifiSignal(row, layout, network.rssi, network.connected ? theme::kPositive : theme::kAccentStrong);
    lv_obj_t* text = CreateSystemColumn(row, 2);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_t* ssid = CreateSystemLabel(text, network.ssid.data(), layout.body_font, theme::kPrimaryText);
    lv_obj_set_width(ssid, LV_PCT(100));
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    (void)CreateSystemLabel(text, ConnectionText(network), layout.detail_font,
                            network.connected ? theme::kPositive : theme::kSecondaryText);
    if (network.secured) {
        (void)CreateSystemLabel(row, kWifiLockSymbol, layout.body_font, theme::kSecondaryText);
    }
    (void)CreateSystemLabel(row, LV_SYMBOL_RIGHT, layout.body_font, theme::kPrimaryText);
}

void WifiSettingsUi::DrawActionSheetLocked() {
    if (selected_saved_index_ >= model_.saved_network_count) {
        action_sheet_visible_ = false;
        return;
    }
    lv_obj_t* overlay = CreateOverlay(root_);
    lv_obj_add_event_cb(overlay, OverlayCancelEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* panel = CreateSystemPanel(overlay, *layout_);
    lv_obj_set_width(panel, layout_->width - layout_->safe_horizontal * 2);
    lv_obj_center(panel);
    const host_ui::WifiNetworkModel& network = model_.saved_networks[selected_saved_index_];
    lv_obj_t* title = CreateSystemLabel(panel, network.ssid.data(), layout_->heading_font, theme::kPrimaryText);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_t* connect = CreateSystemActionButton(panel, *layout_, network.connected ? "Disconnect" : "Connect",
                                                 network.connected ? theme::kDangerSoft : theme::kAccent);
    lv_obj_add_event_cb(connect, SheetConnectEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* forget = CreateSystemActionButton(panel, *layout_, "Forget Network", theme::kDangerSoft);
    lv_obj_add_event_cb(forget, SheetForgetEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* cancel = CreateSystemActionButton(panel, *layout_, "Cancel", theme::kSecondaryText);
    lv_obj_add_event_cb(cancel, OverlayCancelEvent, LV_EVENT_SHORT_CLICKED, this);
}

void WifiSettingsUi::DrawPasswordLocked() {
    lv_obj_t* overlay = CreateOverlay(root_);
    lv_obj_t* dialog = CreateSystemPanel(overlay, *layout_);
    lv_obj_set_width(dialog, layout_->width - layout_->safe_horizontal * 2);
    lv_obj_align(dialog, LV_ALIGN_TOP_MID, 0, layout_->safe_horizontal);
    (void)CreateSystemLabel(dialog, "Wi-Fi Password", layout_->heading_font, theme::kPrimaryText);
    lv_obj_t* ssid =
        CreateSystemLabel(dialog, password_network_.ssid.data(), layout_->detail_font, theme::kSecondaryText);
    lv_obj_set_width(ssid, LV_PCT(100));
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);

    const char* status = nullptr;
    uint32_t status_color = theme::kSecondaryText;
    if (password_length_invalid_) {
        status = "Password must be at least 8 characters";
        status_color = theme::kWifiError;
    } else {
        switch (password_connection_state_) {
            case host_ui::WifiConnectionState::kConnecting:
                status = "Connecting...";
                status_color = theme::kAccent;
                break;
            case host_ui::WifiConnectionState::kAuthenticationFailed:
                status = "Incorrect password. Try again.";
                status_color = theme::kWifiError;
                break;
            case host_ui::WifiConnectionState::kAuthenticationTimedOut:
                status = "Authentication timed out. Try again.";
                status_color = theme::kWifiWarning;
                break;
            case host_ui::WifiConnectionState::kHandshakeTimedOut:
                status = "Security handshake timed out. Try again.";
                status_color = theme::kWifiWarning;
                break;
            case host_ui::WifiConnectionState::kNetworkNotFound:
                status = "Network not found. Try again.";
                status_color = theme::kWifiWarning;
                break;
            case host_ui::WifiConnectionState::kFailed:
                status = "Connection failed. Try again.";
                status_color = theme::kWifiWarning;
                break;
            case host_ui::WifiConnectionState::kDisconnected:
            case host_ui::WifiConnectionState::kConnected:
            default:
                break;
        }
    }
    if (status != nullptr) {
        (void)CreateSystemLabel(dialog, status, layout_->detail_font, status_color);
    }
    password_textarea_ = lv_textarea_create(dialog);
    lv_obj_set_size(password_textarea_, LV_PCT(100), layout_->control_height);
    lv_textarea_set_one_line(password_textarea_, true);
    lv_textarea_set_password_mode(password_textarea_, false);
    lv_textarea_set_max_length(password_textarea_, host_ui::kMaxWifiPasswordLength);
    lv_textarea_set_placeholder_text(password_textarea_, "Password");
    if (password_[0] != '\0') {
        lv_textarea_set_text(password_textarea_, password_.data());
    }
    lv_obj_set_style_text_font(password_textarea_, platform::lvgl::BuiltinLatinFont(layout_->body_font), 0);
    lv_obj_t* actions = CreateSystemColumn(dialog, layout_->panel_gap);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_t* cancel = CreateSystemActionButton(actions, *layout_, "Cancel", theme::kSecondaryText);
    lv_obj_set_width(cancel, 0);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, OverlayCancelEvent, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_t* connect = CreateSystemActionButton(actions, *layout_, "Connect", theme::kAccent);
    lv_obj_set_width(connect, 0);
    lv_obj_set_flex_grow(connect, 1);
    lv_obj_add_event_cb(connect, PasswordConnectEvent, LV_EVENT_SHORT_CLICKED, this);
    keyboard_ = CreateDefaultKeyboard(overlay, password_textarea_, platform::lvgl::SystemFontRole::kSmall);
    lv_obj_set_size(keyboard_, LV_PCT(100), layout_->height * 2 / 5);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(keyboard_, PasswordKeyboardCancelEvent, LV_EVENT_CANCEL, this);
    lv_obj_add_event_cb(keyboard_, PasswordKeyboardReadyEvent, LV_EVENT_READY, this);
    if (password_connection_state_ == host_ui::WifiConnectionState::kConnecting) {
        lv_obj_add_state(connect, LV_STATE_DISABLED);
        SetDefaultKeyboardEditingEnabled(keyboard_, false);
    }
}

void WifiSettingsUi::EmitNetworkAction(host_ui::SystemUiActionType type, const host_ui::WifiNetworkModel& network,
                                       const char* password) {
    if (action_sink_ == nullptr) {
        return;
    }
    host_ui::SystemUiAction action{.type = type};
    action.text = network.ssid;
    if (password != nullptr) {
        std::snprintf(action.secret.data(), action.secret.size(), "%s", password);
    }
    action_sink_(action_context_, action);
}

void WifiSettingsUi::BackEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->action_sink_ == nullptr) {
        return;
    }
    if (ui->scan_view_) {
        ui->scan_view_ = false;
        ui->scroll_offset_ = 0;
        ui->user_scrolled_ = false;
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseWifiNetworkScan});
        ui->QueueRender();
    } else {
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseWifiSettings});
    }
}

void WifiSettingsUi::SwitchEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr) {
        const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
        ui->action_sink_(ui->action_context_, host_ui::SystemUiAction{
                                                  .type = host_ui::SystemUiActionType::kSetWifiEnabled,
                                                  .value = enabled ? 1U : 0U,
                                              });
    }
}

void WifiSettingsUi::OpenScanEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui != nullptr && ui->action_sink_ != nullptr && ui->model_.enabled) {
        ui->scan_view_ = true;
        ui->scroll_offset_ = 0;
        ui->user_scrolled_ = false;
        ui->action_sink_(ui->action_context_,
                         host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kOpenWifiNetworkScan});
        ui->QueueRender();
    }
}

void WifiSettingsUi::NetworkEvent(lv_event_t* event) {
    auto* binding = static_cast<NetworkBinding*>(lv_event_get_user_data(event));
    if (binding == nullptr || binding->ui == nullptr) {
        return;
    }
    WifiSettingsUi& ui = *binding->ui;
    if (binding->saved) {
        if (binding->index >= ui.model_.saved_network_count) {
            return;
        }
        ui.selected_saved_index_ = binding->index;
        ui.action_sheet_visible_ = true;
        ui.QueueRender();
        return;
    }
    if (binding->index >= ui.model_.available_network_count) {
        return;
    }
    const host_ui::WifiNetworkModel& network = ui.model_.available_networks[binding->index];
    if (network.connected) {
        return;
    }
    if (network.saved) {
        ui.EmitNetworkAction(host_ui::SystemUiActionType::kConnectSavedWifi, network);
    } else if (!network.secured) {
        ui.EmitNetworkAction(host_ui::SystemUiActionType::kConnectNewWifi, network);
    } else {
        ui.password_network_ = network;
        ui.password_connection_state_ = host_ui::WifiConnectionState::kDisconnected;
        ui.password_attempt_active_ = false;
        ui.password_length_invalid_ = false;
        ui.password_.fill('\0');
        ui.password_visible_ = true;
        ui.QueueRender();
    }
}

void WifiSettingsUi::SheetConnectEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->selected_saved_index_ >= ui->model_.saved_network_count) {
        return;
    }
    const host_ui::WifiNetworkModel& network = ui->model_.saved_networks[ui->selected_saved_index_];
    ui->EmitNetworkAction(network.connected ? host_ui::SystemUiActionType::kDisconnectWifi
                                            : host_ui::SystemUiActionType::kConnectSavedWifi,
                          network);
    ui->action_sheet_visible_ = false;
    ui->QueueRender();
}

void WifiSettingsUi::SheetForgetEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->selected_saved_index_ >= ui->model_.saved_network_count) {
        return;
    }
    ui->EmitNetworkAction(host_ui::SystemUiActionType::kForgetWifi,
                          ui->model_.saved_networks[ui->selected_saved_index_]);
    ui->action_sheet_visible_ = false;
    ui->QueueRender();
}

void WifiSettingsUi::OverlayCancelEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->action_sheet_visible_ = false;
        ui->password_visible_ = false;
        ui->password_attempt_active_ = false;
        ui->password_length_invalid_ = false;
        ui->password_connection_state_ = host_ui::WifiConnectionState::kDisconnected;
        ui->password_network_ = {};
        ui->password_.fill('\0');
        ui->QueueRender();
    }
}

void WifiSettingsUi::PasswordConnectEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->password_textarea_ == nullptr) {
        return;
    }
    const char* password = lv_textarea_get_text(ui->password_textarea_);
    if (password == nullptr) {
        return;
    }
    std::snprintf(ui->password_.data(), ui->password_.size(), "%s", password);
    if (std::strlen(password) < host_ui::kMinWifiPasswordLength) {
        ui->password_attempt_active_ = false;
        ui->password_length_invalid_ = true;
        ui->password_connection_state_ = host_ui::WifiConnectionState::kDisconnected;
        ui->QueueRender();
        return;
    }
    ui->password_attempt_active_ = true;
    ui->password_length_invalid_ = false;
    ui->password_connection_state_ = host_ui::WifiConnectionState::kConnecting;
    ui->EmitNetworkAction(host_ui::SystemUiActionType::kConnectNewWifi, ui->password_network_, ui->password_.data());
    ui->QueueRender();
}

void WifiSettingsUi::PasswordKeyboardCancelEvent(lv_event_t* event) { OverlayCancelEvent(event); }

void WifiSettingsUi::PasswordKeyboardReadyEvent(lv_event_t* event) { PasswordConnectEvent(event); }

void WifiSettingsUi::ScrollEvent(lv_event_t* event) {
    auto* ui = static_cast<WifiSettingsUi*>(lv_event_get_user_data(event));
    lv_obj_t* content = lv_event_get_current_target_obj(event);
    if (ui == nullptr || content == nullptr) {
        return;
    }
    bool render_after_scroll = false;
    if (lv_event_get_code(event) == LV_EVENT_SCROLL_END) {
        portENTER_CRITICAL(&ui->render_lock_);
        ui->scroll_gesture_active_ = false;
        if (ui->render_pending_) {
            ui->render_pending_ = false;
            render_after_scroll = true;
        }
        portEXIT_CRITICAL(&ui->render_lock_);
    } else {
        lv_indev_t* indev = lv_indev_active();
        if (indev != nullptr && lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            portENTER_CRITICAL(&ui->render_lock_);
            ui->user_scrolled_ = true;
            ui->scroll_gesture_active_ = true;
            portEXIT_CRITICAL(&ui->render_lock_);
        }
    }
    if (ui->user_scrolled_) {
        ui->scroll_offset_ = std::max<int32_t>(0, lv_obj_get_scroll_y(content));
    }
    if (render_after_scroll) {
        ui->QueueRender();
    }
    if (ui->display_ != nullptr) {
        platform::lvgl::RequestDisplayRefresh(ui->display_);
    }
}

void WifiSettingsUi::RenderAsync(void* context) {
    auto* ui = static_cast<WifiSettingsUi*>(context);
    if (ui != nullptr && ui->visible_) {
        ui->RenderLocked();
    }
}

void WifiSettingsUi::QueueRender() { (void)lv_async_call(RenderAsync, this); }

void WifiSettingsUi::Update(const host_ui::WifiSettingsModel& model, bool pointer_busy) {
    model_ = model;
    if (!visible_ || display_ == nullptr || action_sheet_visible_) {
        return;
    }
    if (password_visible_) {
        if (password_attempt_active_ && WifiModelShowsConnectedNetwork(model, password_network_)) {
            password_visible_ = false;
            password_attempt_active_ = false;
            password_length_invalid_ = false;
            password_connection_state_ = host_ui::WifiConnectionState::kDisconnected;
            password_network_ = {};
            password_.fill('\0');
        } else if (!password_attempt_active_) {
            return;
        } else {
            host_ui::WifiConnectionState next_state = password_connection_state_;
            switch (model.connection_state) {
                case host_ui::WifiConnectionState::kConnecting:
                case host_ui::WifiConnectionState::kAuthenticationFailed:
                case host_ui::WifiConnectionState::kAuthenticationTimedOut:
                case host_ui::WifiConnectionState::kHandshakeTimedOut:
                case host_ui::WifiConnectionState::kNetworkNotFound:
                case host_ui::WifiConnectionState::kFailed:
                    next_state = model.connection_state;
                    break;
                case host_ui::WifiConnectionState::kDisconnected:
                case host_ui::WifiConnectionState::kConnected:
                default:
                    break;
            }
            if (next_state == password_connection_state_) {
                return;
            }
            password_connection_state_ = next_state;
        }
    }
    if (pointer_busy || scroll_gesture_active_) {
        portENTER_CRITICAL(&render_lock_);
        render_pending_ = true;
        portEXIT_CRITICAL(&render_lock_);
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    RenderLocked();
    esp_lv_adapter_unlock();
}

void WifiSettingsUi::PointerReleased() {
    bool render = false;
    portENTER_CRITICAL(&render_lock_);
    render = visible_ && render_pending_;
    render_pending_ = false;
    portEXIT_CRITICAL(&render_lock_);
    if (render) {
        QueueRender();
    }
}

void WifiSettingsUi::Leave() {
    visible_ = false;
    action_sink_ = nullptr;
    action_context_ = nullptr;
    raise_overlay_sink_ = nullptr;
    raise_overlay_context_ = nullptr;
    root_ = nullptr;
    display_ = nullptr;
    layout_ = nullptr;
    scroll_content_ = nullptr;
    password_textarea_ = nullptr;
    keyboard_ = nullptr;
    action_sheet_visible_ = false;
    password_visible_ = false;
    password_attempt_active_ = false;
    password_length_invalid_ = false;
    password_connection_state_ = host_ui::WifiConnectionState::kDisconnected;
    password_network_ = {};
    password_.fill('\0');
    scan_view_ = false;
    scroll_offset_ = 0;
    portENTER_CRITICAL(&render_lock_);
    render_pending_ = false;
    user_scrolled_ = false;
    scroll_gesture_active_ = false;
    portEXIT_CRITICAL(&render_lock_);
}

}  // namespace micropixel::host_ui::lvgl::square_common
