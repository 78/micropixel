#pragma once

#include <cstdint>
#include <expected>

#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// Owns the persistent quick-settings layer, its native LVGL controls, and the
// final-display performance HUD. Callers hold the LVGL adapter
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

    void UpdatePerformanceOverlayLocked(bool enabled, uint8_t cpu_percent, uint32_t guest_presented_frame_sequence);
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
        platform::lvgl::SystemFontRole quick_name_font{};
        platform::lvgl::SystemFontRole quick_detail_font{};
        platform::lvgl::SystemFontRole control_font{};
        platform::lvgl::SystemFontRole metric_font{};
        uint32_t scrim_rgb{};
        uint8_t scrim_opacity{};
    };

    struct MetricObjects final {
        lv_obj_t* panel{};
        lv_obj_t* value_label{};
        lv_obj_t* bar{};
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
    [[nodiscard]] static uint8_t SliderMinimum(TouchTarget target);
    [[nodiscard]] static uint8_t ClampSliderValue(TouchTarget target, uint8_t percent);

    void EmitAction(host_ui::SystemUiActionType type, uint32_t value = 0U, uint64_t timestamp_us = 0U);
    void EmitTarget(TouchTarget target, uint64_t timestamp_us = 0U);
    void UpdateSliderLocked(TouchTarget target, uint8_t percent);
    void EmitSliderValue(TouchTarget target, uint8_t percent, uint64_t timestamp_us, bool force);
    [[nodiscard]] TouchTarget QuickTarget(const lv_obj_t* object) const;
    [[nodiscard]] TouchTarget SliderTarget(const lv_obj_t* object) const;

    static void LayerEvent(lv_event_t* event);
    static void QuickEvent(lv_event_t* event);
    static void SliderEvent(lv_event_t* event);

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
    lv_obj_t* quick_name_labels_[3]{};
    lv_obj_t* quick_detail_labels_[3]{};
    lv_obj_t* slider_value_labels_[2]{};
    lv_obj_t* slider_controls_[2]{};
    lv_obj_t* sram_value_label_{};
    lv_obj_t* sram_bar_{};
    lv_obj_t* performance_overlay_{};
    lv_obj_t* performance_label_{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
    uint64_t slider_last_emit_us_[2]{};
    uint8_t slider_values_[2]{};
    uint8_t slider_last_emitted_values_[2]{0xffU, 0xffU};
    uint32_t performance_last_frame_sequence_{};
    uint32_t performance_fps_{};
    int64_t performance_last_sample_us_{};
    bool updating_controls_{};
    const Layout* layout_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
