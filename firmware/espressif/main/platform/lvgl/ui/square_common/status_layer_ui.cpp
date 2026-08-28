#include "platform/lvgl/ui/square_common/status_layer_ui.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::lvgl::square_common {
namespace {

constexpr char kTag[] = "micropixel_status";
constexpr uint32_t kThemeAccentColor = 0x287ee8U;
constexpr uint32_t kStatusControlColor = 0x68a7ffU;
constexpr uint16_t kTransitionComplete = 1000U;

void StyleFullscreenContainer(lv_obj_t* container, uint32_t background, int32_t width, int32_t height) {
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_size(container, width, height);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_bg_color(container, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(container, LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

const StatusLayerUi::Layout& StatusLayerUi::ActiveLayout() {
    static constexpr Layout kLayout720{
        .screen_width = 720,
        .screen_height = 720,
        .dialog = {.x = 24, .y = 36, .width = 672, .height = 508},
        .dialog_hidden_y = -508,
        .quick = {{.x = 48, .y = 52, .width = 196, .height = 108},
                  {.x = 262, .y = 52, .width = 196, .height = 108},
                  {.x = 476, .y = 52, .width = 196, .height = 108}},
        .sliders = {{.x = 48, .y = 184, .width = 624, .height = 80}, {.x = 48, .y = 284, .width = 624, .height = 80}},
        .metrics = {{.x = 48, .y = 390, .width = 192, .height = 124},
                    {.x = 264, .y = 390, .width = 192, .height = 124},
                    {.x = 480, .y = 390, .width = 192, .height = 124}},
        .performance_overlay = {.x = 282, .y = 4, .width = 150, .height = 30},
        .panel_radius = 24,
        .dialog_radius = 30,
        .dialog_border_width = 2,
        .quick_label_x = 18,
        .quick_name_y = 20,
        .quick_detail_y = 67,
        .slider_label_x = 20,
        .slider_label_y = 4,
        .slider_value_width = 44,
        .slider_track_x = 20,
        .slider_track_y = 47,
        .slider_track_height = 14,
        .slider_knob_size = 28,
        .metric_label_x = 16,
        .metric_name_y = 15,
        .metric_value_y = 47,
        .metric_track_x = 16,
        .metric_track_y = 90,
        .metric_track_height = 10,
        .performance_label_y = 7,
        .quick_name_font = SystemFontRole::kLarge,
        .quick_detail_font = SystemFontRole::kMedium,
        .control_font = SystemFontRole::kMedium,
        .metric_font = SystemFontRole::kMedium,
        .scrim_rgb = 0x02060cU,
        .scrim_opacity = 190U,
    };
    static constexpr Layout kLayout480{
        .screen_width = 480,
        .screen_height = 480,
        .dialog = {.x = 16, .y = 16, .width = 448, .height = 380},
        .dialog_hidden_y = -380,
        .quick = {{.x = 32, .y = 32, .width = 128, .height = 72},
                  {.x = 176, .y = 32, .width = 128, .height = 72},
                  {.x = 320, .y = 32, .width = 128, .height = 72}},
        .sliders = {{.x = 32, .y = 124, .width = 200, .height = 118},
                    {.x = 248, .y = 124, .width = 200, .height = 118}},
        .metrics = {{.x = 32, .y = 262, .width = 128, .height = 106},
                    {.x = 176, .y = 262, .width = 128, .height = 106},
                    {.x = 320, .y = 262, .width = 128, .height = 106}},
        .performance_overlay = {.x = 174, .y = 4, .width = 132, .height = 26},
        .panel_radius = 18,
        .dialog_radius = 28,
        .dialog_border_width = 2,
        .quick_label_x = 12,
        .quick_name_y = 10,
        .quick_detail_y = 40,
        .slider_label_x = 16,
        .slider_label_y = 12,
        .slider_value_width = 44,
        .slider_track_x = 24,
        .slider_track_y = 66,
        .slider_track_height = 18,
        .slider_knob_size = 34,
        .metric_label_x = 10,
        .metric_name_y = 12,
        .metric_value_y = 39,
        .metric_track_x = 10,
        .metric_track_y = 82,
        .metric_track_height = 6,
        .performance_label_y = 5,
        .quick_name_font = SystemFontRole::kMedium,
        .quick_detail_font = SystemFontRole::kSmall,
        .control_font = SystemFontRole::kSmall,
        .metric_font = SystemFontRole::kSmall,
        .scrim_rgb = 0x02060cU,
        .scrim_opacity = 190U,
    };
    lv_display_t* display = lv_screen_active() != nullptr ? lv_obj_get_display(lv_screen_active()) : nullptr;
    return display != nullptr && lv_display_get_horizontal_resolution(display) <= 480 ? kLayout480 : kLayout720;
}

void StatusLayerUi::ResolveLayoutLocked() { layout_ = &ActiveLayout(); }

int32_t StatusLayerUi::TransitionDialogVisibleY() const {
    return layout_ != nullptr ? layout_->dialog.y : ActiveLayout().dialog.y;
}

int32_t StatusLayerUi::TransitionDialogHiddenY() const {
    return layout_ != nullptr ? layout_->dialog_hidden_y : ActiveLayout().dialog_hidden_y;
}

uint32_t StatusLayerUi::TransitionScrimRgb() const {
    return layout_ != nullptr ? layout_->scrim_rgb : ActiveLayout().scrim_rgb;
}

uint8_t StatusLayerUi::TransitionScrimOpacity() const {
    return layout_ != nullptr ? layout_->scrim_opacity : ActiveLayout().scrim_opacity;
}

StatusLayerUi::Bounds StatusLayerUi::TargetBounds(TouchTarget target) const {
    if (layout_ == nullptr) {
        return {};
    }
    switch (target) {
        case TouchTarget::kWifi:
            return layout_->quick[0];
        case TouchTarget::kCellular:
            return layout_->quick[1];
        case TouchTarget::kPerformance:
            return layout_->quick[2];
        case TouchTarget::kBrightness:
            return layout_->sliders[0];
        case TouchTarget::kVolume:
            return layout_->sliders[1];
        case TouchTarget::kScrim:
        case TouchTarget::kNone:
            break;
    }
    return {};
}

StatusLayerUi::Bounds StatusLayerUi::DialogRelative(const Bounds& bounds) const {
    return {.x = bounds.x - layout_->dialog.x,
            .y = bounds.y - layout_->dialog.y,
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

StatusLayerUi::TouchTarget StatusLayerUi::FindTouchTarget(int32_t x, int32_t y) const {
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
    return layout_ != nullptr && PointInside(layout_->dialog, x, y) ? TouchTarget::kNone : TouchTarget::kScrim;
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

uint32_t StatusLayerUi::SliderValue(TouchTarget target, int32_t x) const {
    const Bounds bounds = TargetBounds(target);
    const int32_t slider_left = bounds.x + layout_->slider_track_x;
    const int32_t slider_width = bounds.width - 2 * layout_->slider_track_x;
    const int32_t clamped_x =
        x < slider_left ? slider_left : (x > slider_left + slider_width ? slider_left + slider_width : x);
    const uint32_t position_percent = static_cast<uint32_t>(clamped_x - slider_left) * 100U / slider_width;
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
    const int32_t index = SliderIndex(target);
    if (index < 0 || slider_value_labels_[index] == nullptr || slider_fills_[index] == nullptr ||
        slider_knobs_[index] == nullptr) {
        return;
    }
    const uint8_t clamped_percent = ClampSliderValue(target, percent);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_label_set_text(slider_value_labels_[index], value);
    const Bounds bounds = TargetBounds(target);
    const int32_t track_width = bounds.width - 2 * layout_->slider_track_x;
    const int32_t fill_width = static_cast<int32_t>(SliderPositionPercent(target, clamped_percent)) * track_width / 100;
    lv_obj_set_width(slider_fills_[index], fill_width > 0 ? fill_width : 1);
    lv_obj_set_x(slider_knobs_[index], layout_->slider_track_x + fill_width - layout_->slider_knob_size / 2);
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

    const int32_t slider_index = SliderIndex(target);
    if (slider_index >= 0) {
        const uint8_t percent = static_cast<uint8_t>(SliderValue(target, sample.x));
        UpdateSlider(target, percent, false);
        EmitSliderValue(target, percent, sample.timestamp_us, true);
    } else if (target == TouchTarget::kScrim && layout_ != nullptr &&
               !PointInside(layout_->dialog, sample.x, sample.y) && std::abs(delta_x) <= kTapSlop &&
               std::abs(delta_y) <= kTapSlop && elapsed_us <= kGestureTimeoutUs) {
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

lv_obj_t* StatusLayerUi::CreatePanel(lv_obj_t* parent, const Bounds& bounds, uint32_t color) const {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, bounds.x, bounds.y);
    lv_obj_set_size(panel, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, layout_->panel_radius, 0);
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
    (void)CreateLabel(panel, name, BuiltinLatinFont(layout_->quick_name_font), available ? 0xf4f8ffU : 0x708198U,
                      layout_->quick_label_x, layout_->quick_name_y);
    lv_obj_t* detail_label =
        CreateLabel(panel, detail, BuiltinLatinFont(layout_->quick_detail_font),
                    active && available ? 0xf4f8ffU : 0x91a4bdU, layout_->quick_label_x, layout_->quick_detail_y);
    if (index < 0) {
        return;
    }
    lv_obj_t* press_overlay = lv_obj_create(panel);
    lv_obj_set_pos(press_overlay, 0, 0);
    lv_obj_set_size(press_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(press_overlay, 0, 0);
    lv_obj_set_style_radius(press_overlay, layout_->panel_radius, 0);
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
    lv_obj_t* panel = CreatePanel(root, bounds, 0x142235U);
    (void)CreateLabel(panel, name, BuiltinLatinFont(layout_->control_font), 0xf4f8ffU, layout_->slider_label_x,
                      layout_->slider_label_y);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_obj_t* value_label =
        CreateLabel(panel, value, BuiltinLatinFont(layout_->control_font), 0x91a4bdU,
                    bounds.width - layout_->slider_label_x - layout_->slider_value_width, layout_->slider_label_y);
    lv_obj_set_width(value_label, layout_->slider_value_width);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);

    const int32_t track_width = bounds.width - 2 * layout_->slider_track_x;
    lv_obj_t* track = lv_obj_create(panel);
    lv_obj_set_pos(track, layout_->slider_track_x, layout_->slider_track_y);
    lv_obj_set_size(track, track_width, layout_->slider_track_height);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, layout_->slider_track_height / 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x31445bU), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);

    const int32_t fill_width = static_cast<int32_t>(SliderPositionPercent(target, clamped_percent)) * track_width / 100;
    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, fill_width > 0 ? fill_width : 1, layout_->slider_track_height);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, layout_->slider_track_height / 2, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(fill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* knob = lv_obj_create(panel);
    lv_obj_set_pos(knob, layout_->slider_track_x + fill_width - layout_->slider_knob_size / 2,
                   layout_->slider_track_y - (layout_->slider_knob_size - layout_->slider_track_height) / 2);
    lv_obj_set_size(knob, layout_->slider_knob_size, layout_->slider_knob_size);
    lv_obj_set_style_pad_all(knob, 0, 0);
    lv_obj_set_style_border_width(knob, 3, 0);
    lv_obj_set_style_border_color(knob, lv_color_hex(0xf4f8ffU), 0);
    lv_obj_set_style_radius(knob, layout_->slider_knob_size / 2, 0);
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

StatusLayerUi::MetricObjects StatusLayerUi::DrawMetric(lv_obj_t* root, uint32_t index, const char* name,
                                                       const char* value, uint8_t percent, uint32_t color) {
    const Bounds bounds = DialogRelative(layout_->metrics[index]);
    lv_obj_t* panel = CreatePanel(root, bounds, 0x142235U);
    (void)CreateLabel(panel, name, BuiltinLatinFont(layout_->metric_font), 0x91a4bdU, layout_->metric_label_x,
                      layout_->metric_name_y);
    lv_obj_t* value_label = CreateLabel(panel, value, BuiltinLatinFont(layout_->metric_font), 0xf4f8ffU,
                                        layout_->metric_label_x, layout_->metric_value_y);
    lv_obj_t* track = lv_obj_create(panel);
    const int32_t track_width = bounds.width - 2 * layout_->metric_track_x;
    lv_obj_set_pos(track, layout_->metric_track_x, layout_->metric_track_y);
    lv_obj_set_size(track, track_width, layout_->metric_track_height);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, layout_->metric_track_height / 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x31445bU), 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, percent > 0U ? static_cast<int32_t>(percent) * track_width / 100 : 1,
                    layout_->metric_track_height);
    lv_obj_set_style_pad_all(fill, 0, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, layout_->metric_track_height / 2, 0);
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
    const int32_t track_width = layout_->metrics[2].width - 2 * layout_->metric_track_x;
    lv_obj_set_width(sram_fill_, percent > 0U ? static_cast<int32_t>(percent) * track_width / 100 : 1);
}

void StatusLayerUi::DrawLayerLocked(const host_ui::StatusLayerModel& model) {
    ResolveLayoutLocked();
    if (status_layer_ == nullptr) {
        status_layer_ = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(status_layer_, 0x06101cU, layout_->screen_width, layout_->screen_height);
    }
    ResetObjectPointers();
    lv_obj_clean(status_layer_);
    lv_obj_remove_flag(status_layer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(status_layer_, lv_color_hex(layout_->scrim_rgb), 0);
    lv_obj_set_style_bg_opa(status_layer_, layout_->scrim_opacity, 0);

    status_dialog_ = CreatePanel(status_layer_, layout_->dialog, 0x0d1b2cU);
    lv_obj_set_style_radius(status_dialog_, layout_->dialog_radius, 0);
    lv_obj_set_style_border_width(status_dialog_, layout_->dialog_border_width, 0);
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
    const uint32_t memory_used_tenths = (model.memory_used_kib * 10U + 512U) / 1024U;
    (void)std::snprintf(memory, sizeof(memory), "%" PRIu32 ".%" PRIu32 " / %" PRIu32 " MB", memory_used_tenths / 10U,
                        memory_used_tenths % 10U, model.memory_total_kib / 1024U);
    const uint32_t storage_used_tenths = (model.storage_used_kib * 10U + 512U) / 1024U;
    (void)std::snprintf(storage, sizeof(storage), "%" PRIu32 ".%" PRIu32 " / %" PRIu32 " MB", storage_used_tenths / 10U,
                        storage_used_tenths % 10U, model.storage_total_kib / 1024U);
    const uint8_t memory_percent =
        model.memory_total_kib > 0U ? static_cast<uint8_t>(model.memory_used_kib * 100U / model.memory_total_kib) : 0U;
    const uint8_t storage_percent = model.storage_total_kib > 0U
                                        ? static_cast<uint8_t>(model.storage_used_kib * 100U / model.storage_total_kib)
                                        : 0U;
    (void)DrawMetric(status_dialog_, 0U, "STORAGE", storage, storage_percent, kStatusControlColor);
    (void)DrawMetric(status_dialog_, 1U, "MEMORY", memory, memory_percent, kStatusControlColor);
    MetricObjects sram_metric = DrawMetric(status_dialog_, 2U, "SRAM", "", 0U, kStatusControlColor);
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
        layout_->dialog_hidden_y +
        static_cast<int32_t>((layout_->dialog.y - layout_->dialog_hidden_y) * progress / kTransitionComplete);
    lv_obj_set_y(status_dialog_, dialog_y);
    // Keep the scrim stable while the panel moves. Animating a translucent
    // 720x720 object forces LVGL to blend and refresh the full display on
    // every step, which turns the status transition into a slideshow even
    // though the CPU meter remains low while the panel pipeline is blocked.
    lv_obj_set_style_bg_opa(status_layer_, layout_->scrim_opacity, 0);
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
        ResolveLayoutLocked();
        lv_obj_set_pos(performance_overlay_, layout_->performance_overlay.x, layout_->performance_overlay.y);
        lv_obj_set_size(performance_overlay_, layout_->performance_overlay.width, layout_->performance_overlay.height);
        lv_obj_set_style_pad_all(performance_overlay_, 0, 0);
        lv_obj_set_style_radius(performance_overlay_, 6, 0);
        lv_obj_set_style_border_width(performance_overlay_, 0, 0);
        lv_obj_set_style_bg_color(performance_overlay_, lv_color_hex(0x07111eU), 0);
        // This HUD is only 150x30 pixels, so its requested translucent backing
        // remains a small, PPA-eligible blend instead of a full-screen cost.
        lv_obj_set_style_bg_opa(performance_overlay_, 176, 0);
        lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_CLICKABLE);
        performance_label_ = CreateLabel(performance_overlay_, "", BuiltinLatinFont(SystemFontRole::kSmall), 0xf4f8ffU,
                                         0, layout_->performance_label_y);
        lv_obj_set_width(performance_label_, layout_->performance_overlay.width);
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

}  // namespace micropixel::platform::lvgl::square_common
