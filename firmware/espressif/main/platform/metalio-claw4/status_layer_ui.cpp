#include "platform/metalio-claw4/status_layer_ui.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/metalio-claw4/fonts/font_registry.hpp"
#include "platform/metalio-claw4/lvgl_wakeup.hpp"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_status";
constexpr int32_t kScreenWidth = 720;
constexpr int32_t kScreenHeight = 720;
constexpr uint32_t kThemeAccentColor = 0x287ee8U;
constexpr uint32_t kStatusControlColor = 0x68a7ffU;
constexpr int32_t kStatusDialogVisibleY = StatusLayerUi::kTransitionDialogVisibleY;
constexpr int32_t kStatusDialogHiddenY = StatusLayerUi::kTransitionDialogHiddenY;
constexpr uint16_t kTransitionComplete = 1000U;
constexpr lv_opa_t kStatusScrimOpacity = StatusLayerUi::kTransitionScrimOpacity;

void StyleFullscreenContainer(lv_obj_t* container, uint32_t background) {
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, kScreenWidth, kScreenHeight);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

StatusLayerUi::Bounds StatusLayerUi::TargetBounds(TouchTarget target) {
    switch (target) {
        case TouchTarget::kWifi:
            return {.x = 48, .y = 52, .width = 196, .height = 108};
        case TouchTarget::kCellular:
            return {.x = 262, .y = 52, .width = 196, .height = 108};
        case TouchTarget::kPerformance:
            return {.x = 476, .y = 52, .width = 196, .height = 108};
        case TouchTarget::kBrightness:
            return {.x = 48, .y = 184, .width = 624, .height = 80};
        case TouchTarget::kVolume:
            return {.x = 48, .y = 284, .width = 624, .height = 80};
        case TouchTarget::kScrim:
        case TouchTarget::kNone:
            break;
    }
    return {};
}

StatusLayerUi::Bounds StatusLayerUi::DialogRelative(const Bounds& bounds) {
    constexpr Bounds kDialogBounds{.x = 24, .y = kStatusDialogVisibleY, .width = 672, .height = 508};
    return {.x = bounds.x - kDialogBounds.x,
            .y = bounds.y - kDialogBounds.y,
            .width = bounds.width,
            .height = bounds.height};
}

int32_t StatusLayerUi::QuickIndex(TouchTarget target) {
    switch (target) {
        case TouchTarget::kWifi:
            return 0;
        case TouchTarget::kCellular:
            return 1;
        case TouchTarget::kPerformance:
            return 2;
        default:
            return -1;
    }
}

int32_t StatusLayerUi::SliderIndex(TouchTarget target) {
    switch (target) {
        case TouchTarget::kBrightness:
            return 0;
        case TouchTarget::kVolume:
            return 1;
        default:
            return -1;
    }
}

bool StatusLayerUi::PointInside(const Bounds& bounds, int32_t x, int32_t y) {
    return x >= bounds.x && x < bounds.x + bounds.width && y >= bounds.y && y < bounds.y + bounds.height;
}

StatusLayerUi::TouchTarget StatusLayerUi::FindTouchTarget(int32_t x, int32_t y) {
    constexpr TouchTarget kTargets[] = {
        TouchTarget::kWifi,
        TouchTarget::kPerformance,
        TouchTarget::kBrightness,
        TouchTarget::kVolume,
    };
    for (TouchTarget target : kTargets) {
        if (PointInside(TargetBounds(target), x, y)) {
            return target;
        }
    }
    constexpr Bounds kDialogBounds{.x = 24, .y = kStatusDialogVisibleY, .width = 672, .height = 508};
    return PointInside(kDialogBounds, x, y) ? TouchTarget::kNone : TouchTarget::kScrim;
}

uint8_t StatusLayerUi::SliderMinimum(TouchTarget target) {
    return target == TouchTarget::kBrightness ? host_ui::kMinimumBrightnessPercent : 0U;
}

uint8_t StatusLayerUi::ClampSliderValue(TouchTarget target, uint8_t percent) {
    const uint8_t minimum = SliderMinimum(target);
    const uint8_t maximum_clamped = percent <= 100U ? percent : 100U;
    return maximum_clamped >= minimum ? maximum_clamped : minimum;
}

uint32_t StatusLayerUi::SliderPositionPercent(TouchTarget target, uint8_t percent) {
    const uint32_t minimum = SliderMinimum(target);
    const uint32_t clamped = ClampSliderValue(target, percent);
    return (clamped - minimum) * 100U / (100U - minimum);
}

uint32_t StatusLayerUi::SliderValue(TouchTarget target, int32_t x) {
    constexpr int32_t kSliderLeft = 68;
    constexpr int32_t kSliderWidth = 584;
    const int32_t clamped_x =
        x < kSliderLeft ? kSliderLeft : (x > kSliderLeft + kSliderWidth ? kSliderLeft + kSliderWidth : x);
    const uint32_t position_percent = static_cast<uint32_t>(clamped_x - kSliderLeft) * 100U / kSliderWidth;
    const uint32_t minimum = SliderMinimum(target);
    return minimum + position_percent * (100U - minimum) / 100U;
}

void StatusLayerUi::EmitAction(host_ui::SystemUiActionType type, uint32_t value, uint64_t timestamp_us) {
    if (action_sink_ != nullptr) {
        action_sink_(action_context_,
                     host_ui::SystemUiAction{.type = type, .value = value, .timestamp_us = timestamp_us});
    }
}

void StatusLayerUi::EmitTarget(TouchTarget target, int32_t x, uint64_t timestamp_us) {
    switch (target) {
        case TouchTarget::kWifi:
            EmitAction(host_ui::SystemUiActionType::kOpenWifiSettings);
            break;
        case TouchTarget::kCellular:
            // Cellular stays read-only until a real network service owns it.
            break;
        case TouchTarget::kPerformance:
            EmitAction(host_ui::SystemUiActionType::kTogglePerformanceOverlay);
            break;
        case TouchTarget::kBrightness:
            EmitAction(host_ui::SystemUiActionType::kSetBrightness, SliderValue(target, x));
            break;
        case TouchTarget::kVolume:
            EmitAction(host_ui::SystemUiActionType::kSetVolume, SliderValue(target, x));
            break;
        case TouchTarget::kScrim:
            EmitAction(host_ui::SystemUiActionType::kCloseStatusLayer, 0U, timestamp_us);
            break;
        case TouchTarget::kNone:
            break;
    }
}

void StatusLayerUi::SetButtonPressedLocked(TouchTarget target, bool pressed) {
    const int32_t index = QuickIndex(target);
    if (index < 0 || quick_press_overlays_[index] == nullptr) {
        return;
    }
    if (pressed) {
        lv_obj_remove_flag(quick_press_overlays_[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(quick_press_overlays_[index]);
    } else {
        lv_obj_add_flag(quick_press_overlays_[index], LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusLayerUi::SetButtonPressed(TouchTarget target, bool pressed) {
    if (QuickIndex(target) < 0 || status_layer_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    SetButtonPressedLocked(target, pressed);
    RequestDisplayRefresh(lv_obj_get_display(status_layer_));
    esp_lv_adapter_unlock();
}

void StatusLayerUi::UpdateSliderLocked(TouchTarget target, uint8_t percent, bool pressed) {
    constexpr int32_t kTrackWidth = 584;
    const int32_t index = SliderIndex(target);
    if (index < 0 || slider_value_labels_[index] == nullptr || slider_fills_[index] == nullptr ||
        slider_knobs_[index] == nullptr) {
        return;
    }
    const uint8_t clamped_percent = ClampSliderValue(target, percent);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_label_set_text(slider_value_labels_[index], value);
    const int32_t fill_width = static_cast<int32_t>(SliderPositionPercent(target, clamped_percent)) * kTrackWidth / 100;
    lv_obj_set_width(slider_fills_[index], fill_width > 0 ? fill_width : 1);
    const Bounds bounds = DialogRelative(TargetBounds(target));
    lv_obj_set_x(slider_knobs_[index], bounds.x + 20 + fill_width - 14);
    (void)pressed;
    lv_obj_set_style_border_width(slider_knobs_[index], 3, 0);
    slider_values_[index] = clamped_percent;
}

void StatusLayerUi::UpdateSlider(TouchTarget target, uint8_t percent, bool pressed) {
    if (SliderIndex(target) < 0 || status_layer_ == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    UpdateSliderLocked(target, percent, pressed);
    RequestDisplayRefresh(lv_obj_get_display(status_layer_));
    esp_lv_adapter_unlock();
}

void StatusLayerUi::EmitSliderValue(TouchTarget target, uint8_t percent, uint64_t timestamp_us, bool force) {
    constexpr uint64_t kHardwareUpdatePeriodUs = 50000U;
    const int32_t index = SliderIndex(target);
    if (index < 0) {
        return;
    }
    const uint64_t now_us = timestamp_us != 0U ? timestamp_us : static_cast<uint64_t>(esp_timer_get_time());
    const bool changed = slider_last_emitted_values_[index] != percent;
    const bool period_elapsed =
        now_us < slider_last_emit_us_ || now_us - slider_last_emit_us_ >= kHardwareUpdatePeriodUs;
    if (!force && (!changed || !period_elapsed)) {
        return;
    }
    slider_last_emitted_values_[index] = percent;
    slider_last_emit_us_ = now_us;
    EmitAction(target == TouchTarget::kBrightness ? host_ui::SystemUiActionType::kSetBrightness
                                                  : host_ui::SystemUiActionType::kSetVolume,
               percent);
}

bool StatusLayerUi::TouchSink(void* context, const device::TouchSample& sample) {
    auto* ui = static_cast<StatusLayerUi*>(context);
    return ui != nullptr && ui->HandleTouch(sample);
}

bool StatusLayerUi::HandleTouch(const device::TouchSample& sample) {
    if (sample.phase == device::TouchPhase::kDown) {
        touch_target_ = FindTouchTarget(sample.x, sample.y);
        touch_id_ = sample.id;
        touch_down_x_ = sample.x;
        touch_down_y_ = sample.y;
        touch_down_us_ = sample.timestamp_us;
        touch_active_ = true;
        button_pressed_ = QuickIndex(touch_target_) >= 0;
        if (button_pressed_) {
            SetButtonPressed(touch_target_, true);
        }
        const int32_t slider_index = SliderIndex(touch_target_);
        if (slider_index >= 0) {
            const uint8_t percent = static_cast<uint8_t>(SliderValue(touch_target_, sample.x));
            UpdateSlider(touch_target_, percent, true);
            EmitSliderValue(touch_target_, percent, sample.timestamp_us, true);
        }
        return true;
    }
    if (!touch_active_ || touch_id_ != sample.id) {
        return true;
    }

    const TouchTarget target = touch_target_;
    const int32_t delta_x = sample.x - touch_down_x_;
    const int32_t delta_y = sample.y - touch_down_y_;
    if (sample.phase == device::TouchPhase::kCancel) {
        if (button_pressed_) {
            SetButtonPressed(target, false);
        }
        const int32_t slider_index = SliderIndex(target);
        if (slider_index >= 0) {
            UpdateSlider(target, slider_values_[slider_index], false);
            EmitSliderValue(target, slider_values_[slider_index], sample.timestamp_us, true);
        }
        touch_active_ = false;
        button_pressed_ = false;
        touch_target_ = TouchTarget::kNone;
        return true;
    }
    if (sample.phase == device::TouchPhase::kMove) {
        const int32_t quick_index = QuickIndex(target);
        if (quick_index >= 0) {
            const bool pressed = PointInside(TargetBounds(target), sample.x, sample.y);
            if (pressed != button_pressed_) {
                button_pressed_ = pressed;
                SetButtonPressed(target, pressed);
            }
        }
        const int32_t slider_index = SliderIndex(target);
        if (slider_index >= 0) {
            const uint8_t percent = static_cast<uint8_t>(SliderValue(target, sample.x));
            if (slider_values_[slider_index] != percent) {
                UpdateSlider(target, percent, true);
            }
            EmitSliderValue(target, percent, sample.timestamp_us, false);
        }
        return true;
    }
    if (sample.phase != device::TouchPhase::kUp) {
        return true;
    }

    constexpr int32_t kTapSlop = 24;
    constexpr int32_t kSwipeDistance = 56;
    constexpr int32_t kSwipeDirectionSlop = 80;
    constexpr uint64_t kGestureTimeoutUs = 800000U;
    const uint64_t elapsed_us =
        sample.timestamp_us >= touch_down_us_ ? sample.timestamp_us - touch_down_us_ : kGestureTimeoutUs + 1U;
    const bool swipe_up =
        delta_y <= -kSwipeDistance && std::abs(delta_x) <= kSwipeDirectionSlop && elapsed_us <= kGestureTimeoutUs;
    if (button_pressed_ || QuickIndex(target) >= 0) {
        SetButtonPressed(target, false);
    }
    touch_active_ = false;
    button_pressed_ = false;
    touch_target_ = TouchTarget::kNone;
    if (swipe_up) {
        EmitAction(host_ui::SystemUiActionType::kCloseStatusLayer, 0U, sample.timestamp_us);
        return true;
    }

    constexpr Bounds kDialogBounds{.x = 24, .y = kStatusDialogVisibleY, .width = 672, .height = 508};
    const int32_t slider_index = SliderIndex(target);
    if (slider_index >= 0) {
        const uint8_t percent = static_cast<uint8_t>(SliderValue(target, sample.x));
        UpdateSlider(target, percent, false);
        EmitSliderValue(target, percent, sample.timestamp_us, true);
    } else if (target == TouchTarget::kScrim && !PointInside(kDialogBounds, sample.x, sample.y) &&
               std::abs(delta_x) <= kTapSlop && std::abs(delta_y) <= kTapSlop && elapsed_us <= kGestureTimeoutUs) {
        EmitTarget(target, sample.x, sample.timestamp_us);
    } else if (QuickIndex(target) >= 0 && PointInside(TargetBounds(target), sample.x, sample.y) &&
               std::abs(delta_x) <= kTapSlop && std::abs(delta_y) <= kTapSlop && elapsed_us <= kGestureTimeoutUs) {
        EmitTarget(target, sample.x, sample.timestamp_us);
    }
    return true;
}

lv_obj_t* StatusLayerUi::CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color,
                                     int32_t x, int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

lv_obj_t* StatusLayerUi::CreatePanel(lv_obj_t* parent, const Bounds& bounds, uint32_t color) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, bounds.x, bounds.y);
    lv_obj_set_size(panel, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x42607fU), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

void StatusLayerUi::DrawQuickCard(lv_obj_t* root, TouchTarget target, const char* name, const char* detail, bool active,
                                  bool available) {
    const uint32_t color = !available ? 0x182331U : (active ? kThemeAccentColor : 0x26384dU);
    const int32_t index = QuickIndex(target);
    lv_obj_t* panel = CreatePanel(root, DialogRelative(TargetBounds(target)), color);
    lv_obj_set_style_bg_opa(panel, available && !active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    (void)CreateLabel(panel, name, BuiltinLatinFont(SystemFontRole::kLarge), available ? 0xf4f8ffU : 0x708198U, 18, 20);
    lv_obj_t* detail_label = CreateLabel(panel, detail, BuiltinLatinFont(SystemFontRole::kMedium),
                                         active && available ? 0xf4f8ffU : 0x91a4bdU, 18, 67);
    if (index < 0) {
        return;
    }
    lv_obj_t* press_overlay = lv_obj_create(panel);
    lv_obj_set_pos(press_overlay, 0, 0);
    lv_obj_set_size(press_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(press_overlay, 0, 0);
    lv_obj_set_style_radius(press_overlay, 24, 0);
    lv_obj_set_style_border_width(press_overlay, 0, 0);
    lv_obj_set_style_bg_color(press_overlay, lv_color_hex(0x000000U), 0);
    lv_obj_set_style_bg_opa(press_overlay, 92, 0);
    lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(press_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(press_overlay, LV_OBJ_FLAG_HIDDEN);
    quick_panels_[index] = panel;
    quick_detail_labels_[index] = detail_label;
    quick_press_overlays_[index] = press_overlay;
}

void StatusLayerUi::DrawSlider(lv_obj_t* root, TouchTarget target, const char* name, uint8_t percent, uint32_t color) {
    const Bounds bounds = DialogRelative(TargetBounds(target));
    const int32_t index = SliderIndex(target);
    const uint8_t clamped_percent = ClampSliderValue(target, percent);
    (void)CreateLabel(root, name, BuiltinLatinFont(SystemFontRole::kMedium), 0xf4f8ffU, bounds.x + 20, bounds.y + 4);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_obj_t* value_label = CreateLabel(root, value, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU,
                                        bounds.x + bounds.width - 62, bounds.y + 4);

    constexpr int32_t kTrackLeftInset = 20;
    constexpr int32_t kTrackWidth = 584;
    constexpr int32_t kTrackHeight = 14;
    const int32_t track_y = bounds.y + 47;
    lv_obj_t* track = lv_obj_create(root);
    lv_obj_set_pos(track, bounds.x + kTrackLeftInset, track_y);
    lv_obj_set_size(track, kTrackWidth, kTrackHeight);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 7, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x31445bU), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    const int32_t fill_width = static_cast<int32_t>(SliderPositionPercent(target, clamped_percent)) * kTrackWidth / 100;
    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, fill_width > 0 ? fill_width : 1, kTrackHeight);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 7, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* knob = lv_obj_create(root);
    lv_obj_set_pos(knob, bounds.x + kTrackLeftInset + fill_width - 12, track_y - 7);
    lv_obj_set_size(knob, 28, 28);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_set_style_border_width(knob, 3, 0);
    lv_obj_set_style_border_color(knob, lv_color_hex(0xf4f8ffU), 0);
    lv_obj_set_style_radius(knob, 14, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(color), 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    if (index >= 0) {
        slider_value_labels_[index] = value_label;
        slider_fills_[index] = fill;
        slider_knobs_[index] = knob;
        slider_values_[index] = clamped_percent;
        slider_last_emitted_values_[index] = clamped_percent;
    }
}

StatusLayerUi::MetricObjects StatusLayerUi::DrawMetric(lv_obj_t* root, int32_t x, const char* name, const char* value,
                                                       uint8_t percent, uint32_t color) {
    constexpr int32_t kDialogX = 24;
    constexpr int32_t kDialogY = 36;
    const Bounds bounds{.x = x - kDialogX, .y = 390 - kDialogY, .width = 192, .height = 124};
    lv_obj_t* panel = CreatePanel(root, bounds, 0x142235U);
    (void)CreateLabel(panel, name, BuiltinLatinFont(SystemFontRole::kMedium), 0x91a4bdU, 16, 15);
    lv_obj_t* value_label = CreateLabel(panel, value, BuiltinLatinFont(SystemFontRole::kMedium), 0xf4f8ffU, 16, 47);
    lv_obj_t* track = lv_obj_create(panel);
    lv_obj_set_pos(track, 16, 90);
    lv_obj_set_size(track, 160, 10);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 5, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x31445bU), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, percent > 0U ? static_cast<int32_t>(percent) * 160 / 100 : 1, 10);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 5, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    return {.panel = panel, .value_label = value_label, .fill = fill};
}

void StatusLayerUi::ResetObjectPointers() {
    status_dialog_ = nullptr;
    for (uint32_t index = 0U; index < 3U; ++index) {
        quick_panels_[index] = nullptr;
        quick_detail_labels_[index] = nullptr;
        quick_press_overlays_[index] = nullptr;
    }
    for (uint32_t index = 0U; index < 2U; ++index) {
        slider_value_labels_[index] = nullptr;
        slider_fills_[index] = nullptr;
        slider_knobs_[index] = nullptr;
    }
    sram_value_label_ = nullptr;
    sram_fill_ = nullptr;
}

void StatusLayerUi::UpdateQuickCardLocked(TouchTarget target, const char* detail, bool active, bool available) {
    const int32_t index = QuickIndex(target);
    if (index < 0 || quick_panels_[index] == nullptr || quick_detail_labels_[index] == nullptr) {
        return;
    }
    const uint32_t color = !available ? 0x182331U : (active ? kThemeAccentColor : 0x26384dU);
    lv_obj_set_style_bg_color(quick_panels_[index], lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(quick_panels_[index], available && !active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_label_set_text(quick_detail_labels_[index], detail);
    lv_obj_set_style_text_color(quick_detail_labels_[index], lv_color_hex(active && available ? 0xf4f8ffU : 0x91a4bdU),
                                0);
}

void StatusLayerUi::UpdateControlsLocked(const host_ui::StatusLayerModel& model) {
    const char* wifi_detail = !model.wifi_available ? "UNAVAILABLE"
                                                    : (model.wifi_connected    ? "CONNECTED"
                                                       : model.wifi_connecting ? "CONNECTING"
                                                       : model.wifi_enabled    ? "ON"
                                                                               : "OFF");
    const char* cellular_detail =
        !model.cellular_available ? "UNAVAILABLE"
                                  : (model.cellular_connected ? "CONNECTED" : (model.cellular_enabled ? "ON" : "OFF"));
    UpdateQuickCardLocked(TouchTarget::kWifi, wifi_detail, model.wifi_enabled, model.wifi_available);
    UpdateQuickCardLocked(TouchTarget::kCellular, cellular_detail, model.cellular_enabled, model.cellular_available);
    UpdateQuickCardLocked(TouchTarget::kPerformance,
                          model.performance_overlay_enabled ? "FPS + CPU ON" : "FPS + CPU OFF",
                          model.performance_overlay_enabled, true);
    UpdateSliderLocked(TouchTarget::kBrightness, model.brightness_percent, false);
    UpdateSliderLocked(TouchTarget::kVolume, model.volume_percent, false);
    UpdateSramMetricLocked(model);
}

void StatusLayerUi::UpdateSramMetricLocked(const host_ui::StatusLayerModel& model) {
    if (sram_value_label_ == nullptr || sram_fill_ == nullptr) {
        return;
    }
    char sram[24]{};
    (void)std::snprintf(sram, sizeof(sram), "%" PRIu32 " / %" PRIu32 " KB", model.sram_used_kib, model.sram_total_kib);
    lv_label_set_text(sram_value_label_, sram);
    const uint8_t percent =
        model.sram_total_kib > 0U ? static_cast<uint8_t>(model.sram_used_kib * 100U / model.sram_total_kib) : 0U;
    lv_obj_set_width(sram_fill_, percent > 0U ? static_cast<int32_t>(percent) * 160 / 100 : 1);
}

void StatusLayerUi::DrawLayerLocked(const host_ui::StatusLayerModel& model) {
    if (status_layer_ == nullptr) {
        status_layer_ = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(status_layer_, 0x06101cU);
    }
    ResetObjectPointers();
    lv_obj_clean(status_layer_);
    lv_obj_remove_flag(status_layer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(status_layer_, lv_color_hex(kTransitionScrimRgb), 0);
    lv_obj_set_style_bg_opa(status_layer_, kStatusScrimOpacity, 0);

    constexpr Bounds kDialogBounds{.x = 24, .y = kStatusDialogVisibleY, .width = 672, .height = 508};
    status_dialog_ = CreatePanel(status_layer_, kDialogBounds, 0x0d1b2cU);
    lv_obj_set_style_radius(status_dialog_, 30, 0);
    lv_obj_set_style_border_width(status_dialog_, 2, 0);
    lv_obj_set_style_border_color(status_dialog_, lv_color_hex(0x496786U), 0);

    const char* wifi_detail = !model.wifi_available ? "UNAVAILABLE"
                                                    : (model.wifi_connected    ? "CONNECTED"
                                                       : model.wifi_connecting ? "CONNECTING"
                                                       : model.wifi_enabled    ? "ON"
                                                                               : "OFF");
    const char* cellular_detail =
        !model.cellular_available ? "UNAVAILABLE"
                                  : (model.cellular_connected ? "CONNECTED" : (model.cellular_enabled ? "ON" : "OFF"));
    DrawQuickCard(status_dialog_, TouchTarget::kWifi, "WIFI", wifi_detail, model.wifi_enabled, model.wifi_available);
    DrawQuickCard(status_dialog_, TouchTarget::kCellular, "4G", cellular_detail, model.cellular_enabled,
                  model.cellular_available);
    DrawQuickCard(status_dialog_, TouchTarget::kPerformance, "FPS",
                  model.performance_overlay_enabled ? "FPS + CPU ON" : "FPS + CPU OFF",
                  model.performance_overlay_enabled, true);

    DrawSlider(status_dialog_, TouchTarget::kBrightness, "BRIGHTNESS", model.brightness_percent, kStatusControlColor);
    DrawSlider(status_dialog_, TouchTarget::kVolume, "VOLUME", model.volume_percent, kStatusControlColor);

    char memory[24]{};
    char storage[24]{};
    (void)std::snprintf(memory, sizeof(memory), "%" PRIu32 " / %" PRIu32 " MB", model.memory_used_kib / 1024U,
                        model.memory_total_kib / 1024U);
    const uint32_t storage_used_tenths = (model.storage_used_kib * 10U + 512U) / 1024U;
    (void)std::snprintf(storage, sizeof(storage), "%" PRIu32 ".%" PRIu32 " / %" PRIu32 " MB", storage_used_tenths / 10U,
                        storage_used_tenths % 10U, model.storage_total_kib / 1024U);
    const uint8_t memory_percent =
        model.memory_total_kib > 0U ? static_cast<uint8_t>(model.memory_used_kib * 100U / model.memory_total_kib) : 0U;
    const uint8_t storage_percent = model.storage_total_kib > 0U
                                        ? static_cast<uint8_t>(model.storage_used_kib * 100U / model.storage_total_kib)
                                        : 0U;
    (void)DrawMetric(status_dialog_, 48, "STORAGE", storage, storage_percent, kStatusControlColor);
    (void)DrawMetric(status_dialog_, 264, "MEMORY", memory, memory_percent, kStatusControlColor);
    MetricObjects sram_metric = DrawMetric(status_dialog_, 480, "SRAM", "", 0U, kStatusControlColor);
    sram_value_label_ = sram_metric.value_label;
    sram_fill_ = sram_metric.fill;
    UpdateSramMetricLocked(model);

    lv_obj_move_foreground(status_layer_);
    RaisePerformanceOverlayLocked();
    RequestDisplayRefresh(lv_obj_get_display(status_layer_));
}

std::expected<void, host_ui::SystemUiError> StatusLayerUi::ShowLocked(const host_ui::StatusLayerModel& model,
                                                                      host_ui::SystemUiActionSink action_sink,
                                                                      void* action_context) {
    if (lv_screen_active() == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    DrawLayerLocked(model);
    SetTransitionProgressLocked(0U);
    action_sink_ = action_sink;
    action_context_ = action_context;
    touch_active_ = false;
    button_pressed_ = false;
    touch_target_ = TouchTarget::kNone;
    ESP_LOGI(kTag, "status layer visible");
    return {};
}

void StatusLayerUi::UpdateLocked(const host_ui::StatusLayerModel& model) {
    if (status_layer_ == nullptr || lv_obj_has_flag(status_layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    UpdateControlsLocked(model);
    RequestDisplayRefresh(lv_obj_get_display(status_layer_));
}

void StatusLayerUi::SetTransitionProgressLocked(uint16_t progress_per_mille) {
    if (status_layer_ == nullptr || status_dialog_ == nullptr || lv_obj_has_flag(status_layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const uint32_t progress = progress_per_mille <= kTransitionComplete ? progress_per_mille : kTransitionComplete;
    const int32_t dialog_y =
        kStatusDialogHiddenY +
        static_cast<int32_t>((kStatusDialogVisibleY - kStatusDialogHiddenY) * progress / kTransitionComplete);
    lv_obj_set_y(status_dialog_, dialog_y);
    // Keep the scrim stable while the panel moves. Animating a translucent
    // 720x720 object forces LVGL to blend and refresh the full display on
    // every step, which turns the status transition into a slideshow even
    // though the CPU meter remains low while the panel pipeline is blocked.
    lv_obj_set_style_bg_opa(status_layer_, kStatusScrimOpacity, 0);
    RaisePerformanceOverlayLocked();
    RequestDisplayRefresh(lv_obj_get_display(status_layer_));
}

void StatusLayerUi::Deactivate() {
    action_sink_ = nullptr;
    action_context_ = nullptr;
    touch_active_ = false;
    button_pressed_ = false;
    touch_target_ = TouchTarget::kNone;
}

void StatusLayerUi::LeaveLocked() {
    Deactivate();
    if (status_layer_ == nullptr) {
        return;
    }
    lv_obj_clean(status_layer_);
    ResetObjectPointers();
    lv_obj_add_flag(status_layer_, LV_OBJ_FLAG_HIDDEN);
    RequestDisplayRefresh(lv_obj_get_display(status_layer_));
    ESP_LOGI(kTag, "status layer hidden");
}

void* StatusLayerUi::ActionContext() const { return action_context_; }

void StatusLayerUi::RaisePerformanceOverlayLocked() {
    if (PerformanceOverlayVisibleLocked()) {
        lv_obj_move_foreground(performance_overlay_);
    }
}

bool StatusLayerUi::PerformanceOverlayVisibleLocked() const {
    return performance_overlay_ != nullptr && !lv_obj_has_flag(performance_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void StatusLayerUi::UpdatePerformanceOverlayLocked(bool enabled, uint8_t cpu_percent,
                                                   uint32_t display_refresh_sequence) {
    if (!enabled) {
        if (performance_overlay_ != nullptr) {
            lv_obj_add_flag(performance_overlay_, LV_OBJ_FLAG_HIDDEN);
            RequestDisplayRefresh(lv_obj_get_display(performance_overlay_));
        }
        performance_last_frame_sequence_ = display_refresh_sequence;
        performance_last_sample_us_ = 0;
        performance_fps_ = 0U;
        return;
    }

    if (performance_overlay_ == nullptr) {
        performance_overlay_ = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(performance_overlay_, 282, 4);
        lv_obj_set_size(performance_overlay_, 150, 30);
        lv_obj_set_style_pad_all(performance_overlay_, 0, 0);
        lv_obj_set_style_radius(performance_overlay_, 6, 0);
        lv_obj_set_style_border_width(performance_overlay_, 0, 0);
        lv_obj_set_style_bg_color(performance_overlay_, lv_color_hex(0x07111eU), 0);
        // This HUD is only 150x30 pixels, so its requested translucent backing
        // remains a small, PPA-eligible blend instead of a full-screen cost.
        lv_obj_set_style_bg_opa(performance_overlay_, 176, 0);
        lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_CLICKABLE);
        performance_label_ =
            CreateLabel(performance_overlay_, "", BuiltinLatinFont(SystemFontRole::kSmall), 0xf4f8ffU, 0, 7);
        lv_obj_set_width(performance_label_, 150);
        lv_obj_set_style_text_align(performance_label_, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_HIDDEN);

    const int64_t now_us = esp_timer_get_time();
    if (performance_last_sample_us_ != 0 && now_us > performance_last_sample_us_) {
        const uint32_t frames = display_refresh_sequence - performance_last_frame_sequence_;
        const uint64_t elapsed_us = static_cast<uint64_t>(now_us - performance_last_sample_us_);
        performance_fps_ =
            static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000000U + elapsed_us / 2U) / elapsed_us);
    }
    performance_last_frame_sequence_ = display_refresh_sequence;
    performance_last_sample_us_ = now_us;
    char performance_text[32]{};
    (void)std::snprintf(performance_text, sizeof(performance_text), "CPU %u%%     FPS %" PRIu32,
                        static_cast<unsigned>(cpu_percent), performance_fps_);
    lv_label_set_text(performance_label_, performance_text);
    RaisePerformanceOverlayLocked();
    RequestDisplayRefresh(lv_obj_get_display(performance_overlay_));
}

}  // namespace micropixel::platform::metalio_claw4
