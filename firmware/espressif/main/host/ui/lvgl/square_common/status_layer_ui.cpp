#include "host/ui/lvgl/square_common/status_layer_ui.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

constexpr char kTag[] = "micropixel_status";
constexpr uint16_t kTransitionComplete = 1000U;

constexpr int32_t Scale480To720(int32_t value) { return value * 3 / 2; }

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
        .performance_label_y = 3,
        .quick_name_font = platform::lvgl::SystemFontRole::kMedium,
        .quick_detail_font = platform::lvgl::SystemFontRole::kSmall,
        .control_font = platform::lvgl::SystemFontRole::kSmall,
        .metric_font = platform::lvgl::SystemFontRole::kSmall,
        .scrim_rgb = theme::kStatusScrim,
        .scrim_opacity = 190U,
    };
    // Keep the 720px presentation structurally identical to the compact 480px
    // layout. In particular, the two controls stay side-by-side instead of
    // switching to a separate wide-screen composition.
    static constexpr Layout kLayout720{
        .screen_width = Scale480To720(kLayout480.screen_width),
        .screen_height = Scale480To720(kLayout480.screen_height),
        .dialog = {.x = Scale480To720(kLayout480.dialog.x),
                   .y = Scale480To720(kLayout480.dialog.y),
                   .width = Scale480To720(kLayout480.dialog.width),
                   .height = Scale480To720(kLayout480.dialog.height)},
        .dialog_hidden_y = Scale480To720(kLayout480.dialog_hidden_y),
        .quick = {{.x = Scale480To720(kLayout480.quick[0].x),
                   .y = Scale480To720(kLayout480.quick[0].y),
                   .width = Scale480To720(kLayout480.quick[0].width),
                   .height = Scale480To720(kLayout480.quick[0].height)},
                  {.x = Scale480To720(kLayout480.quick[1].x),
                   .y = Scale480To720(kLayout480.quick[1].y),
                   .width = Scale480To720(kLayout480.quick[1].width),
                   .height = Scale480To720(kLayout480.quick[1].height)},
                  {.x = Scale480To720(kLayout480.quick[2].x),
                   .y = Scale480To720(kLayout480.quick[2].y),
                   .width = Scale480To720(kLayout480.quick[2].width),
                   .height = Scale480To720(kLayout480.quick[2].height)}},
        .sliders = {{.x = Scale480To720(kLayout480.sliders[0].x),
                     .y = Scale480To720(kLayout480.sliders[0].y),
                     .width = Scale480To720(kLayout480.sliders[0].width),
                     .height = Scale480To720(kLayout480.sliders[0].height)},
                    {.x = Scale480To720(kLayout480.sliders[1].x),
                     .y = Scale480To720(kLayout480.sliders[1].y),
                     .width = Scale480To720(kLayout480.sliders[1].width),
                     .height = Scale480To720(kLayout480.sliders[1].height)}},
        .metrics = {{.x = Scale480To720(kLayout480.metrics[0].x),
                     .y = Scale480To720(kLayout480.metrics[0].y),
                     .width = Scale480To720(kLayout480.metrics[0].width),
                     .height = Scale480To720(kLayout480.metrics[0].height)},
                    {.x = Scale480To720(kLayout480.metrics[1].x),
                     .y = Scale480To720(kLayout480.metrics[1].y),
                     .width = Scale480To720(kLayout480.metrics[1].width),
                     .height = Scale480To720(kLayout480.metrics[1].height)},
                    {.x = Scale480To720(kLayout480.metrics[2].x),
                     .y = Scale480To720(kLayout480.metrics[2].y),
                     .width = Scale480To720(kLayout480.metrics[2].width),
                     .height = Scale480To720(kLayout480.metrics[2].height)}},
        .performance_overlay = {.x = Scale480To720(kLayout480.performance_overlay.x),
                                .y = Scale480To720(kLayout480.performance_overlay.y),
                                .width = Scale480To720(kLayout480.performance_overlay.width),
                                .height = Scale480To720(kLayout480.performance_overlay.height)},
        .panel_radius = Scale480To720(kLayout480.panel_radius),
        .dialog_radius = Scale480To720(kLayout480.dialog_radius),
        .dialog_border_width = Scale480To720(kLayout480.dialog_border_width),
        .quick_label_x = Scale480To720(kLayout480.quick_label_x),
        .quick_name_y = Scale480To720(kLayout480.quick_name_y),
        .quick_detail_y = Scale480To720(kLayout480.quick_detail_y),
        .slider_label_x = Scale480To720(kLayout480.slider_label_x),
        .slider_label_y = Scale480To720(kLayout480.slider_label_y),
        .slider_value_width = Scale480To720(kLayout480.slider_value_width),
        .slider_track_x = Scale480To720(kLayout480.slider_track_x),
        .slider_track_y = Scale480To720(kLayout480.slider_track_y),
        .slider_track_height = Scale480To720(kLayout480.slider_track_height),
        .slider_knob_size = Scale480To720(kLayout480.slider_knob_size),
        .metric_label_x = Scale480To720(kLayout480.metric_label_x),
        .metric_name_y = Scale480To720(kLayout480.metric_name_y),
        .metric_value_y = Scale480To720(kLayout480.metric_value_y),
        .metric_track_x = Scale480To720(kLayout480.metric_track_x),
        .metric_track_y = Scale480To720(kLayout480.metric_track_y),
        .metric_track_height = Scale480To720(kLayout480.metric_track_height),
        .performance_label_y = Scale480To720(kLayout480.performance_label_y),
        .quick_name_font = platform::lvgl::SystemFontRole::kLarge,
        .quick_detail_font = platform::lvgl::SystemFontRole::kMedium,
        .control_font = platform::lvgl::SystemFontRole::kMedium,
        .metric_font = platform::lvgl::SystemFontRole::kMedium,
        .scrim_rgb = kLayout480.scrim_rgb,
        .scrim_opacity = kLayout480.scrim_opacity,
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

uint8_t StatusLayerUi::SliderMinimum(TouchTarget target) {
    return target == TouchTarget::kBrightness ? host_ui::kMinimumBrightnessPercent : 0U;
}

uint8_t StatusLayerUi::ClampSliderValue(TouchTarget target, uint8_t percent) {
    const uint8_t minimum = SliderMinimum(target);
    const uint8_t maximum_clamped = percent <= 100U ? percent : 100U;
    return maximum_clamped >= minimum ? maximum_clamped : minimum;
}

void StatusLayerUi::EmitAction(host_ui::SystemUiActionType type, uint32_t value, uint64_t timestamp_us) {
    if (action_sink_ != nullptr) {
        action_sink_(action_context_,
                     host_ui::SystemUiAction{.type = type, .value = value, .timestamp_us = timestamp_us});
    }
}

void StatusLayerUi::EmitTarget(TouchTarget target, uint64_t timestamp_us) {
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
        case TouchTarget::kVolume:
            // Native sliders emit their values from SliderEvent.
            break;
        case TouchTarget::kScrim:
            EmitAction(host_ui::SystemUiActionType::kCloseStatusLayer, 0U, timestamp_us);
            break;
        case TouchTarget::kNone:
            break;
    }
}

void StatusLayerUi::UpdateSliderLocked(TouchTarget target, uint8_t percent) {
    const int32_t index = SliderIndex(target);
    if (index < 0 || slider_value_labels_[index] == nullptr || slider_controls_[index] == nullptr) {
        return;
    }
    const uint8_t clamped_percent = ClampSliderValue(target, percent);
    updating_controls_ = true;
    lv_slider_set_value(slider_controls_[index], clamped_percent, LV_ANIM_OFF);
    updating_controls_ = false;
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_label_set_text(slider_value_labels_[index], value);
    slider_values_[index] = clamped_percent;
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
        now_us < slider_last_emit_us_[index] || now_us - slider_last_emit_us_[index] >= kHardwareUpdatePeriodUs;
    if (!force && (!changed || !period_elapsed)) {
        return;
    }
    slider_last_emitted_values_[index] = percent;
    slider_last_emit_us_[index] = now_us;
    EmitAction(target == TouchTarget::kBrightness ? host_ui::SystemUiActionType::kSetBrightness
                                                  : host_ui::SystemUiActionType::kSetVolume,
               percent);
}

StatusLayerUi::TouchTarget StatusLayerUi::QuickTarget(const lv_obj_t* object) const {
    static constexpr TouchTarget kQuickTargets[]{
        TouchTarget::kWifi,
        TouchTarget::kCellular,
        TouchTarget::kPerformance,
    };
    for (int32_t index = 0; index < 3; ++index) {
        if (quick_panels_[index] == object) {
            return kQuickTargets[index];
        }
    }
    return TouchTarget::kNone;
}

StatusLayerUi::TouchTarget StatusLayerUi::SliderTarget(const lv_obj_t* object) const {
    if (slider_controls_[0] == object) {
        return TouchTarget::kBrightness;
    }
    if (slider_controls_[1] == object) {
        return TouchTarget::kVolume;
    }
    return TouchTarget::kNone;
}

void StatusLayerUi::LayerEvent(lv_event_t* event) {
    auto* ui = static_cast<StatusLayerUi*>(lv_event_get_user_data(event));
    if (ui == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SHORT_CLICKED && lv_event_get_target_obj(event) == ui->status_layer_) {
        ui->EmitTarget(TouchTarget::kScrim, static_cast<uint64_t>(esp_timer_get_time()));
    } else if (code == LV_EVENT_GESTURE) {
        lv_indev_t* indev = lv_event_get_indev(event);
        if (indev != nullptr && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
            ui->EmitTarget(TouchTarget::kScrim, static_cast<uint64_t>(esp_timer_get_time()));
        }
    }
}

void StatusLayerUi::QuickEvent(lv_event_t* event) {
    auto* ui = static_cast<StatusLayerUi*>(lv_event_get_user_data(event));
    if (ui != nullptr) {
        ui->EmitTarget(ui->QuickTarget(lv_event_get_target_obj(event)));
    }
}

void StatusLayerUi::SliderEvent(lv_event_t* event) {
    auto* ui = static_cast<StatusLayerUi*>(lv_event_get_user_data(event));
    if (ui == nullptr || ui->updating_controls_) {
        return;
    }
    lv_obj_t* slider = lv_event_get_target_obj(event);
    const TouchTarget target = ui->SliderTarget(slider);
    const int32_t index = SliderIndex(target);
    if (index < 0) {
        return;
    }
    const uint8_t percent = static_cast<uint8_t>(lv_slider_get_value(slider));
    ui->slider_values_[index] = percent;
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(percent));
    lv_label_set_text(ui->slider_value_labels_[index], value);
    const lv_event_code_t code = lv_event_get_code(event);
    ui->EmitSliderValue(target, percent, static_cast<uint64_t>(esp_timer_get_time()), code != LV_EVENT_VALUE_CHANGED);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(slider));
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
    lv_obj_set_style_border_color(panel, lv_color_hex(theme::kStrongBorder), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    return panel;
}

void StatusLayerUi::DrawQuickCard(lv_obj_t* root, TouchTarget target, const char* name, const char* detail, bool active,
                                  bool available) {
    const uint32_t color = !available ? theme::kDisabledControlBackground
                                      : (active ? theme::kAccentStrong : theme::kInactiveControlBackground);
    const int32_t index = QuickIndex(target);
    const Bounds bounds = DialogRelative(TargetBounds(target));
    lv_obj_t* panel = lv_button_create(root);
    lv_obj_set_pos(panel, bounds.x, bounds.y);
    lv_obj_set_size(panel, bounds.width, bounds.height);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_radius(panel, layout_->panel_radius, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(theme::kStrongBorder), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, available && !active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(panel, 180, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(panel, 3, LV_STATE_PRESSED);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    const uint32_t name_color =
        !available ? theme::kDisabledText : (active ? theme::kOverlayText : theme::kPrimaryText);
    lv_obj_t* name_label = CreateLabel(panel, name, platform::lvgl::BuiltinLatinFont(layout_->quick_name_font),
                                       name_color, layout_->quick_label_x, layout_->quick_name_y);
    lv_obj_t* detail_label = CreateLabel(panel, detail, platform::lvgl::BuiltinLatinFont(layout_->quick_detail_font),
                                         active && available ? theme::kOverlayText : theme::kSecondaryText,
                                         layout_->quick_label_x, layout_->quick_detail_y);
    if (index < 0) {
        return;
    }
    quick_panels_[index] = panel;
    quick_name_labels_[index] = name_label;
    quick_detail_labels_[index] = detail_label;
    const bool interactive = target != TouchTarget::kCellular && available;
    lv_obj_add_event_cb(panel, QuickEvent, LV_EVENT_SHORT_CLICKED, this);
    if (!interactive) {
        lv_obj_add_state(panel, LV_STATE_DISABLED);
    }
}

void StatusLayerUi::DrawSlider(lv_obj_t* root, TouchTarget target, const char* name, uint8_t percent, uint32_t color) {
    const Bounds bounds = DialogRelative(TargetBounds(target));
    const int32_t index = SliderIndex(target);
    const uint8_t clamped_percent = ClampSliderValue(target, percent);
    lv_obj_t* panel = CreatePanel(root, bounds, theme::kStatusPanelBackground);
    (void)CreateLabel(panel, name, platform::lvgl::BuiltinLatinFont(layout_->control_font), theme::kPrimaryText,
                      layout_->slider_label_x, layout_->slider_label_y);
    char value[8]{};
    (void)std::snprintf(value, sizeof(value), "%u%%", static_cast<unsigned>(clamped_percent));
    lv_obj_t* value_label =
        CreateLabel(panel, value, platform::lvgl::BuiltinLatinFont(layout_->control_font), theme::kSecondaryText,
                    bounds.width - layout_->slider_label_x - layout_->slider_value_width, layout_->slider_label_y);
    lv_obj_set_width(value_label, layout_->slider_value_width);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);

    const int32_t track_width = bounds.width - 2 * layout_->slider_track_x;
    lv_obj_t* slider = lv_slider_create(panel);
    lv_obj_set_pos(slider, layout_->slider_track_x, layout_->slider_track_y);
    lv_obj_set_size(slider, track_width, layout_->slider_track_height);
    lv_slider_set_range(slider, SliderMinimum(target), 100);
    lv_slider_set_value(slider, clamped_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::kControlTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, layout_->slider_track_height / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, layout_->slider_track_height / 2, LV_PART_INDICATOR);
    lv_obj_set_style_width(slider, layout_->slider_knob_size, LV_PART_KNOB);
    lv_obj_set_style_height(slider, layout_->slider_knob_size, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(color), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_border_width(
        slider, 4, static_cast<lv_style_selector_t>(LV_PART_KNOB) | static_cast<lv_style_selector_t>(LV_STATE_PRESSED));
    lv_obj_set_style_border_color(slider, lv_color_hex(theme::kOverlayText), LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, (layout_->slider_knob_size - layout_->slider_track_height) / 2 + 8);
    lv_obj_add_event_cb(slider, SliderEvent, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider, SliderEvent, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(slider, SliderEvent, LV_EVENT_PRESS_LOST, this);
    if (index >= 0) {
        slider_value_labels_[index] = value_label;
        slider_controls_[index] = slider;
        slider_values_[index] = clamped_percent;
        slider_last_emitted_values_[index] = clamped_percent;
    }
}

StatusLayerUi::MetricObjects StatusLayerUi::DrawMetric(lv_obj_t* root, uint32_t index, const char* name,
                                                       const char* value, uint8_t percent, uint32_t color) {
    const Bounds bounds = DialogRelative(layout_->metrics[index]);
    lv_obj_t* panel = CreatePanel(root, bounds, theme::kStatusPanelBackground);
    (void)CreateLabel(panel, name, platform::lvgl::BuiltinLatinFont(layout_->metric_font), theme::kSecondaryText,
                      layout_->metric_label_x, layout_->metric_name_y);
    lv_obj_t* value_label = CreateLabel(panel, value, platform::lvgl::BuiltinLatinFont(layout_->metric_font),
                                        theme::kPrimaryText, layout_->metric_label_x, layout_->metric_value_y);
    lv_obj_t* bar = lv_bar_create(panel);
    const int32_t track_width = bounds.width - 2 * layout_->metric_track_x;
    lv_obj_set_pos(bar, layout_->metric_track_x, layout_->metric_track_y);
    lv_obj_set_size(bar, track_width, layout_->metric_track_height);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, std::min<uint8_t>(percent, 100U), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::kControlTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, layout_->metric_track_height / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, layout_->metric_track_height / 2, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return {.panel = panel, .value_label = value_label, .bar = bar};
}

void StatusLayerUi::ResetObjectPointers() {
    status_dialog_ = nullptr;
    for (uint32_t index = 0U; index < 3U; ++index) {
        quick_panels_[index] = nullptr;
        quick_name_labels_[index] = nullptr;
        quick_detail_labels_[index] = nullptr;
    }
    for (uint32_t index = 0U; index < 2U; ++index) {
        slider_value_labels_[index] = nullptr;
        slider_controls_[index] = nullptr;
    }
    sram_value_label_ = nullptr;
    sram_bar_ = nullptr;
}

void StatusLayerUi::UpdateQuickCardLocked(TouchTarget target, const char* detail, bool active, bool available) {
    const int32_t index = QuickIndex(target);
    if (index < 0 || quick_panels_[index] == nullptr || quick_name_labels_[index] == nullptr ||
        quick_detail_labels_[index] == nullptr) {
        return;
    }
    const uint32_t color = !available ? theme::kDisabledControlBackground
                                      : (active ? theme::kAccentStrong : theme::kInactiveControlBackground);
    lv_obj_set_style_bg_color(quick_panels_[index], lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(quick_panels_[index], available && !active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    const uint32_t name_color =
        !available ? theme::kDisabledText : (active ? theme::kOverlayText : theme::kPrimaryText);
    lv_obj_set_style_text_color(quick_name_labels_[index], lv_color_hex(name_color), 0);
    lv_label_set_text(quick_detail_labels_[index], detail);
    lv_obj_set_style_text_color(quick_detail_labels_[index],
                                lv_color_hex(active && available ? theme::kOverlayText : theme::kSecondaryText), 0);
    const bool interactive = target != TouchTarget::kCellular && available;
    if (interactive) {
        lv_obj_remove_state(quick_panels_[index], LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(quick_panels_[index], LV_STATE_DISABLED);
    }
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
    UpdateSliderLocked(TouchTarget::kBrightness, model.brightness_percent);
    UpdateSliderLocked(TouchTarget::kVolume, model.volume_percent);
    UpdateSramMetricLocked(model);
}

void StatusLayerUi::UpdateSramMetricLocked(const host_ui::StatusLayerModel& model) {
    if (sram_value_label_ == nullptr || sram_bar_ == nullptr) {
        return;
    }
    char sram[24]{};
    (void)std::snprintf(sram, sizeof(sram), "%" PRIu32 " / %" PRIu32 " KB", model.sram_used_kib, model.sram_total_kib);
    lv_label_set_text(sram_value_label_, sram);
    const uint8_t percent =
        model.sram_total_kib > 0U ? static_cast<uint8_t>(model.sram_used_kib * 100U / model.sram_total_kib) : 0U;
    lv_bar_set_value(sram_bar_, std::min<uint8_t>(percent, 100U), LV_ANIM_OFF);
}

void StatusLayerUi::DrawLayerLocked(const host_ui::StatusLayerModel& model) {
    ResolveLayoutLocked();
    if (status_layer_ == nullptr) {
        status_layer_ = lv_obj_create(lv_screen_active());
        StyleFullscreenContainer(status_layer_, theme::kStatusLayerBackground, layout_->screen_width,
                                 layout_->screen_height);
        lv_obj_add_flag(status_layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(status_layer_, LayerEvent, LV_EVENT_SHORT_CLICKED, this);
        lv_obj_add_event_cb(status_layer_, LayerEvent, LV_EVENT_GESTURE, this);
    }
    ResetObjectPointers();
    lv_obj_clean(status_layer_);
    lv_obj_remove_flag(status_layer_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(status_layer_, lv_color_hex(layout_->scrim_rgb), 0);
    lv_obj_set_style_bg_opa(status_layer_, layout_->scrim_opacity, 0);

    status_dialog_ = CreatePanel(status_layer_, layout_->dialog, theme::kStatusDialogBackground);
    // Consume taps on empty dialog space so only the surrounding scrim closes
    // the layer. Child controls keep LV_OBJ_FLAG_GESTURE_BUBBLE so an upward
    // gesture can still dismiss the sheet through LayerEvent.
    lv_obj_add_flag(status_dialog_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(status_dialog_, layout_->dialog_radius, 0);
    lv_obj_set_style_border_width(status_dialog_, layout_->dialog_border_width, 0);
    lv_obj_set_style_border_color(status_dialog_, lv_color_hex(theme::kStatusDialogBorder), 0);

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

    DrawSlider(status_dialog_, TouchTarget::kBrightness, "BRIGHTNESS", model.brightness_percent, theme::kStatusControl);
    DrawSlider(status_dialog_, TouchTarget::kVolume, "VOLUME", model.volume_percent, theme::kStatusControl);

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
    (void)DrawMetric(status_dialog_, 0U, "STORAGE", storage, storage_percent, theme::kStatusControl);
    (void)DrawMetric(status_dialog_, 1U, "MEMORY", memory, memory_percent, theme::kStatusControl);
    MetricObjects sram_metric = DrawMetric(status_dialog_, 2U, "SRAM", "", 0U, theme::kStatusControl);
    sram_value_label_ = sram_metric.value_label;
    sram_bar_ = sram_metric.bar;
    UpdateSramMetricLocked(model);

    lv_obj_move_foreground(status_layer_);
    RaisePerformanceOverlayLocked();
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(status_layer_));
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
    updating_controls_ = false;
    ESP_LOGI(kTag, "status layer visible");
    return {};
}

void StatusLayerUi::UpdateLocked(const host_ui::StatusLayerModel& model) {
    if (status_layer_ == nullptr || lv_obj_has_flag(status_layer_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    UpdateControlsLocked(model);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(status_layer_));
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
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(status_layer_));
}

void StatusLayerUi::Deactivate() {
    action_sink_ = nullptr;
    action_context_ = nullptr;
    updating_controls_ = false;
}

void StatusLayerUi::LeaveLocked() {
    Deactivate();
    if (status_layer_ == nullptr) {
        return;
    }
    lv_obj_clean(status_layer_);
    ResetObjectPointers();
    lv_obj_add_flag(status_layer_, LV_OBJ_FLAG_HIDDEN);
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(status_layer_));
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
                                                   uint32_t guest_presented_frame_sequence) {
    if (!enabled) {
        if (performance_overlay_ != nullptr) {
            lv_obj_add_flag(performance_overlay_, LV_OBJ_FLAG_HIDDEN);
            platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(performance_overlay_));
        }
        performance_last_frame_sequence_ = guest_presented_frame_sequence;
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
        lv_obj_set_style_bg_color(performance_overlay_, lv_color_hex(theme::kPerformanceOverlayBackground), 0);
        // This HUD is only 150x30 pixels, so its requested translucent backing
        // remains a small, PPA-eligible blend instead of a full-screen cost.
        lv_obj_set_style_bg_opa(performance_overlay_, 176, 0);
        lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_CLICKABLE);
        performance_label_ = CreateLabel(performance_overlay_, "",
                                         platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kSmall),
                                         theme::kPrimaryText, 0, layout_->performance_label_y);
        lv_obj_set_width(performance_label_, layout_->performance_overlay.width);
        lv_obj_set_style_text_align(performance_label_, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_remove_flag(performance_overlay_, LV_OBJ_FLAG_HIDDEN);

    const int64_t now_us = esp_timer_get_time();
    if (performance_last_sample_us_ != 0 && now_us > performance_last_sample_us_) {
        const uint32_t frames = guest_presented_frame_sequence - performance_last_frame_sequence_;
        const uint64_t elapsed_us = static_cast<uint64_t>(now_us - performance_last_sample_us_);
        performance_fps_ =
            static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000000U + elapsed_us / 2U) / elapsed_us);
    }
    performance_last_frame_sequence_ = guest_presented_frame_sequence;
    performance_last_sample_us_ = now_us;
    char performance_text[32]{};
    (void)std::snprintf(performance_text, sizeof(performance_text), "CPU %u%%  FPS %" PRIu32,
                        static_cast<unsigned>(cpu_percent), performance_fps_);
    lv_label_set_text(performance_label_, performance_text);
    RaisePerformanceOverlayLocked();
    platform::lvgl::RequestDisplayRefresh(lv_obj_get_display(performance_overlay_));
}

}  // namespace micropixel::host_ui::lvgl::square_common
