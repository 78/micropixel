#include "platform/metalio-claw4/system_menu_ui.hpp"

#include <cinttypes>
#include <cstdlib>

#include "esp_log.h"
#include "esp_lv_adapter.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "system_menu_ui";

struct Bounds final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

bool PointInside(const Bounds& bounds, int32_t x, int32_t y) {
    return x >= bounds.x && y >= bounds.y && x < bounds.x + bounds.width && y < bounds.y + bounds.height;
}

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int32_t x, int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* CreatePressOverlay(lv_obj_t* parent, int32_t radius) {
    lv_obj_t* overlay = lv_obj_create(parent);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, radius, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    return overlay;
}

const char* WifiDetail(const host_ui::SystemMenuModel& model) {
    return !model.wifi_available   ? "Not available"
           : model.wifi_connected  ? "Connected"
           : model.wifi_connecting ? "Connecting..."
           : model.wifi_enabled    ? "Not connected"
                                   : "Off";
}

}  // namespace

struct SystemMenuUiAccess final {
    using TouchTarget = SystemMenuUi::TouchTarget;

    static Bounds TargetBounds(TouchTarget target) {
        switch (target) {
            case TouchTarget::kBack:
                return {.x = 24, .y = 24, .width = 88, .height = 88};
            case TouchTarget::kWifi:
                return {.x = 32, .y = 140, .width = 656, .height = 120};
            case TouchTarget::kLanguage:
                return {.x = 32, .y = 380, .width = 656, .height = 120};
            case TouchTarget::kSystemInformation:
                return {.x = 32, .y = 260, .width = 656, .height = 120};
            case TouchTarget::kManageApps:
                return {.x = 32, .y = 500, .width = 656, .height = 120};
            case TouchTarget::kNone:
                break;
        }
        return {};
    }

    static int32_t TargetIndex(TouchTarget target) {
        switch (target) {
            case TouchTarget::kBack:
                return 0;
            case TouchTarget::kWifi:
                return 1;
            case TouchTarget::kLanguage:
                return 2;
            case TouchTarget::kSystemInformation:
                return 3;
            case TouchTarget::kManageApps:
                return 4;
            case TouchTarget::kNone:
                break;
        }
        return -1;
    }

    static TouchTarget FindTouchTarget(int32_t x, int32_t y) {
        constexpr TouchTarget kTargets[] = {
            TouchTarget::kBack,       TouchTarget::kWifi, TouchTarget::kLanguage, TouchTarget::kSystemInformation,
            TouchTarget::kManageApps,
        };
        for (TouchTarget target : kTargets) {
            if (PointInside(TargetBounds(target), x, y)) {
                return target;
            }
        }
        return TouchTarget::kNone;
    }

    static host_ui::SystemMenuItem MenuItem(TouchTarget target) {
        switch (target) {
            case TouchTarget::kLanguage:
                return host_ui::SystemMenuItem::kLanguage;
            case TouchTarget::kSystemInformation:
                return host_ui::SystemMenuItem::kSystemInformation;
            case TouchTarget::kManageApps:
                return host_ui::SystemMenuItem::kManageApps;
            case TouchTarget::kWifi:
            case TouchTarget::kBack:
            case TouchTarget::kNone:
                return host_ui::SystemMenuItem::kWifi;
        }
        return host_ui::SystemMenuItem::kWifi;
    }

    static void DrawRow(SystemMenuUi& state, lv_obj_t* root, TouchTarget target, const char* icon_text,
                        const char* name, const char* detail, uint32_t icon_color) {
        const Bounds target_bounds = TargetBounds(target);
        const Bounds bounds{.x = 40, .y = target_bounds.y + 8, .width = 640, .height = target_bounds.height - 16};
        lv_obj_t* panel = lv_obj_create(root);
        lv_obj_set_pos(panel, bounds.x, bounds.y);
        lv_obj_set_size(panel, bounds.width, bounds.height);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_set_style_radius(panel, 22, 0);
        lv_obj_set_style_border_width(panel, 1, 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(target == TouchTarget::kWifi ? 0x42607fU : 0x2e4562U), 0);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x111f32U), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* icon = lv_label_create(panel);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(icon_color), 0);
        lv_obj_set_size(icon, 56, 38);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(icon, 26, 32);
        (void)CreateLabel(panel, name, &lv_font_montserrat_24, 0xf2f7ffU, 104, 20);
        lv_obj_t* detail_label = CreateLabel(panel, detail, &lv_font_montserrat_18, 0x91a4bdU, 104, 59);
        if (target == TouchTarget::kWifi) {
            state.wifi_detail_label_ = detail_label;
        }
        lv_obj_t* chevron = lv_label_create(panel);
        lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(chevron, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(chevron, lv_color_hex(0xf2f7ffU), 0);
        lv_obj_set_pos(chevron, bounds.width - 46, 38);

        const int32_t index = TargetIndex(target);
        if (index >= 0) {
            state.press_overlays_[index] = CreatePressOverlay(panel, 22);
        }
    }
};

void SystemMenuUi::ResetObjectPointers() {
    wifi_detail_label_ = nullptr;
    for (lv_obj_t*& overlay : press_overlays_) {
        overlay = nullptr;
    }
}

void SystemMenuUi::Update(const host_ui::SystemMenuModel& model) {
    if (wifi_detail_label_ == nullptr || display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    lv_label_set_text(wifi_detail_label_, WifiDetail(model));
    lv_timer_ready(lv_display_get_refr_timer(display_));
    esp_lv_adapter_unlock();
}

void SystemMenuUi::SetTargetPressed(TouchTarget target, bool pressed) {
    const int32_t index = SystemMenuUiAccess::TargetIndex(target);
    if (index < 0 || press_overlays_[index] == nullptr || display_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    lv_obj_t* overlay = press_overlays_[index];
    const bool already_pressed = !lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    if (already_pressed != pressed) {
        if (pressed) {
            lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(overlay);
        } else {
            lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        }
        lv_timer_ready(lv_display_get_refr_timer(display_));
    }
    esp_lv_adapter_unlock();
}

bool SystemMenuUi::TouchSink(void* context, const device::TouchSample& sample) {
    auto* state = static_cast<SystemMenuUi*>(context);
    return state == nullptr ? false : state->HandleTouch(sample);
}

bool SystemMenuUi::HandleTouch(const device::TouchSample& sample) {
    if (sample.phase == device::TouchPhase::kDown) {
        touch_target_ = SystemMenuUiAccess::FindTouchTarget(sample.x, sample.y);
        touch_id_ = sample.id;
        touch_down_x_ = sample.x;
        touch_down_y_ = sample.y;
        touch_down_us_ = sample.timestamp_us;
        touch_active_ = touch_target_ != TouchTarget::kNone;
        button_pressed_ = touch_active_;
        if (touch_active_) {
            SetTargetPressed(touch_target_, true);
        }
        return true;
    }
    if (!touch_active_ || touch_id_ != sample.id) {
        return true;
    }

    const TouchTarget target = touch_target_;
    if (sample.phase == device::TouchPhase::kMove) {
        const bool pressed = PointInside(SystemMenuUiAccess::TargetBounds(target), sample.x, sample.y);
        if (pressed != button_pressed_) {
            button_pressed_ = pressed;
            SetTargetPressed(target, pressed);
        }
        return true;
    }
    if (sample.phase == device::TouchPhase::kCancel) {
        SetTargetPressed(target, false);
        touch_active_ = false;
        button_pressed_ = false;
        touch_target_ = TouchTarget::kNone;
        return true;
    }
    if (sample.phase != device::TouchPhase::kUp) {
        return true;
    }

    constexpr int32_t kTapSlop = 24;
    constexpr uint64_t kTapTimeoutUs = 800000U;
    const int32_t delta_x = sample.x - touch_down_x_;
    const int32_t delta_y = sample.y - touch_down_y_;
    const uint64_t elapsed_us =
        sample.timestamp_us >= touch_down_us_ ? sample.timestamp_us - touch_down_us_ : kTapTimeoutUs + 1U;
    const bool accepted = button_pressed_ &&
                          PointInside(SystemMenuUiAccess::TargetBounds(target), sample.x, sample.y) &&
                          std::abs(delta_x) <= kTapSlop && std::abs(delta_y) <= kTapSlop && elapsed_us <= kTapTimeoutUs;
    SetTargetPressed(target, false);
    touch_active_ = false;
    button_pressed_ = false;
    touch_target_ = TouchTarget::kNone;
    if (!accepted || action_sink_ == nullptr) {
        return true;
    }
    if (target == TouchTarget::kBack) {
        action_sink_(action_context_, host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kCloseSystemMenu});
    } else {
        action_sink_(action_context_,
                     host_ui::SystemUiAction{.type = host_ui::SystemUiActionType::kSelectSystemMenuItem,
                                             .value = static_cast<uint32_t>(SystemMenuUiAccess::MenuItem(target))});
    }
    return true;
}

std::expected<void, host_ui::SystemUiError> SystemMenuUi::ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                     const host_ui::SystemMenuModel& model,
                                                                     host_ui::SystemUiActionSink action_sink,
                                                                     void* action_context) {
    if (root == nullptr || display == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    display_ = display;
    ResetObjectPointers();

    lv_obj_t* back = lv_obj_create(root);
    lv_obj_set_pos(back, 40, 40);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0d1929U), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_90, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xf2f7ffU), 0);
    lv_obj_center(back_label);
    press_overlays_[0] = CreatePressOverlay(back, 18);

    (void)CreateLabel(root, "System Settings", &lv_font_montserrat_32, 0xf2f7ffU, 116, 36);
    (void)CreateLabel(root, "Configure this device", &lv_font_montserrat_18, 0x91a4bdU, 116, 78);

    SystemMenuUiAccess::DrawRow(*this, root, TouchTarget::kWifi, LV_SYMBOL_WIFI, "Wi-Fi", WifiDetail(model), 0x69a7ffU);
    SystemMenuUiAccess::DrawRow(*this, root, TouchTarget::kSystemInformation, "i", "System Information",
                                "Device and software", 0x69a7ffU);
    SystemMenuUiAccess::DrawRow(*this, root, TouchTarget::kLanguage, "A", "Language",
                                model.language != nullptr ? model.language : "English", 0x4dd6a4U);
    SystemMenuUiAccess::DrawRow(*this, root, TouchTarget::kManageApps, LV_SYMBOL_LIST, "Manage Apps",
                                model.installed_app_count == 1U ? "1 installed app" : "Installed apps", 0xff9f43U);

    lv_obj_move_foreground(root);
    lv_timer_ready(lv_display_get_refr_timer(display_));
    action_sink_ = action_sink;
    action_context_ = action_context;
    touch_active_ = false;
    button_pressed_ = false;
    touch_target_ = TouchTarget::kNone;
    ESP_LOGI(kTag, "System Settings visible: wifi=%s apps=%" PRIu32,
             model.wifi_available ? (model.wifi_connected ? "connected" : "available") : "unavailable",
             model.installed_app_count);
    return {};
}

void SystemMenuUi::Deactivate() {
    action_sink_ = nullptr;
    action_context_ = nullptr;
    touch_active_ = false;
    button_pressed_ = false;
    touch_target_ = TouchTarget::kNone;
    ResetObjectPointers();
    display_ = nullptr;
}

}  // namespace micropixel::platform::metalio_claw4
