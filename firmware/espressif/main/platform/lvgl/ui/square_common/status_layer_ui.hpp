#pragma once

#include <cstdint>
#include <expected>

#include "device/input.hpp"
#include "host_ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::platform::lvgl::square_common {

// Owns the persistent quick-settings layer, its manual touch interaction
// state, and the final-display performance HUD. Callers hold the LVGL adapter
// lock for methods whose name ends in Locked.
class StatusLayerUi final {
   public:
    StatusLayerUi() = default;
    StatusLayerUi(const StatusLayerUi&) = delete;
    StatusLayerUi& operator=(const StatusLayerUi&) = delete;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(const host_ui::StatusLayerModel& model,
                                                                         host_ui::SystemUiActionSink action_sink,
                                                                         void* action_context);
    void UpdateLocked(const host_ui::StatusLayerModel& model);
    void SetTransitionProgressLocked(uint16_t progress_per_mille);
    void Deactivate();
    void LeaveLocked();
    [[nodiscard]] void* ActionContext() const;
    [[nodiscard]] lv_obj_t* TransitionDialogLocked() const { return status_dialog_; }
    [[nodiscard]] int32_t TransitionDialogVisibleY() const;
    [[nodiscard]] int32_t TransitionDialogHiddenY() const;
    [[nodiscard]] uint32_t TransitionScrimRgb() const;
    [[nodiscard]] uint8_t TransitionScrimOpacity() const;

    static bool TouchSink(void* context, const device::TouchSample& sample);

    void UpdatePerformanceOverlayLocked(bool enabled, uint8_t cpu_percent, uint32_t display_refresh_sequence);
    void RaisePerformanceOverlayLocked();
    [[nodiscard]] bool PerformanceOverlayVisibleLocked() const;

   private:
    struct Bounds final {
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};
    };

    struct Layout final {
        int32_t screen_width{};
        int32_t screen_height{};
        Bounds dialog{};
        int32_t dialog_hidden_y{};
        Bounds quick[3]{};
        Bounds sliders[2]{};
        Bounds metrics[3]{};
        Bounds performance_overlay{};
        int32_t panel_radius{};
        int32_t dialog_radius{};
        int32_t dialog_border_width{};
        int32_t quick_label_x{};
        int32_t quick_name_y{};
        int32_t quick_detail_y{};
        int32_t slider_label_x{};
        int32_t slider_label_y{};
        int32_t slider_value_width{};
        int32_t slider_track_x{};
        int32_t slider_track_y{};
        int32_t slider_track_height{};
        int32_t slider_knob_size{};
        int32_t metric_label_x{};
        int32_t metric_name_y{};
        int32_t metric_value_y{};
        int32_t metric_track_x{};
        int32_t metric_track_y{};
        int32_t metric_track_height{};
        int32_t performance_label_y{};
        SystemFontRole quick_name_font{};
        SystemFontRole quick_detail_font{};
        SystemFontRole control_font{};
        SystemFontRole metric_font{};
        uint32_t scrim_rgb{};
        uint8_t scrim_opacity{};
    };

    struct MetricObjects final {
        lv_obj_t* panel{};
        lv_obj_t* value_label{};
        lv_obj_t* fill{};
    };

    enum class TouchTarget : uint8_t {
        kNone,
        kScrim,
        kWifi,
        kCellular,
        kPerformance,
        kBrightness,
        kVolume,
    };

    [[nodiscard]] Bounds TargetBounds(TouchTarget target) const;
    [[nodiscard]] Bounds DialogRelative(const Bounds& bounds) const;
    [[nodiscard]] static int32_t QuickIndex(TouchTarget target);
    [[nodiscard]] static int32_t SliderIndex(TouchTarget target);
    [[nodiscard]] TouchTarget FindTouchTarget(int32_t x, int32_t y) const;
    [[nodiscard]] static uint8_t SliderMinimum(TouchTarget target);
    [[nodiscard]] static uint8_t ClampSliderValue(TouchTarget target, uint8_t percent);
    [[nodiscard]] static uint32_t SliderPositionPercent(TouchTarget target, uint8_t percent);
    [[nodiscard]] uint32_t SliderValue(TouchTarget target, int32_t x) const;
    [[nodiscard]] static bool PointInside(const Bounds& bounds, int32_t x, int32_t y);

    void EmitAction(host_ui::SystemUiActionType type, uint32_t value = 0U, uint64_t timestamp_us = 0U);
    void EmitTarget(TouchTarget target, int32_t x, uint64_t timestamp_us);
    void SetButtonPressedLocked(TouchTarget target, bool pressed);
    void SetButtonPressed(TouchTarget target, bool pressed);
    void UpdateSliderLocked(TouchTarget target, uint8_t percent, bool pressed);
    void UpdateSlider(TouchTarget target, uint8_t percent, bool pressed);
    void EmitSliderValue(TouchTarget target, uint8_t percent, uint64_t timestamp_us, bool force);
    [[nodiscard]] bool HandleTouch(const device::TouchSample& sample);

    [[nodiscard]] static lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                                               uint32_t color, int32_t x, int32_t y);
    [[nodiscard]] lv_obj_t* CreatePanel(lv_obj_t* parent, const Bounds& bounds, uint32_t color) const;
    void DrawQuickCard(lv_obj_t* root, TouchTarget target, const char* name, const char* detail, bool active,
                       bool available);
    void DrawSlider(lv_obj_t* root, TouchTarget target, const char* name, uint8_t percent, uint32_t color);
    [[nodiscard]] MetricObjects DrawMetric(lv_obj_t* root, uint32_t index, const char* name, const char* value,
                                           uint8_t percent, uint32_t color);
    void ResetObjectPointers();
    void UpdateQuickCardLocked(TouchTarget target, const char* detail, bool active, bool available);
    void UpdateControlsLocked(const host_ui::StatusLayerModel& model);
    void UpdateSramMetricLocked(const host_ui::StatusLayerModel& model);
    void DrawLayerLocked(const host_ui::StatusLayerModel& model);
    [[nodiscard]] static const Layout& ActiveLayout();
    void ResolveLayoutLocked();

    lv_obj_t* status_layer_{};
    lv_obj_t* status_dialog_{};
    lv_obj_t* quick_panels_[3]{};
    lv_obj_t* quick_detail_labels_[3]{};
    lv_obj_t* quick_press_overlays_[3]{};
    lv_obj_t* slider_value_labels_[2]{};
    lv_obj_t* slider_fills_[2]{};
    lv_obj_t* slider_knobs_[2]{};
    lv_obj_t* sram_value_label_{};
    lv_obj_t* sram_fill_{};
    lv_obj_t* performance_overlay_{};
    lv_obj_t* performance_label_{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
    uint32_t touch_id_{};
    TouchTarget touch_target_{TouchTarget::kNone};
    int32_t touch_down_x_{};
    int32_t touch_down_y_{};
    uint64_t touch_down_us_{};
    uint64_t slider_last_emit_us_{};
    uint8_t slider_values_[2]{};
    uint8_t slider_last_emitted_values_[2]{0xffU, 0xffU};
    uint32_t performance_last_frame_sequence_{};
    uint32_t performance_fps_{};
    int64_t performance_last_sample_us_{};
    bool touch_active_{};
    bool button_pressed_{};
    const Layout* layout_{};
};

}  // namespace micropixel::platform::lvgl::square_common
