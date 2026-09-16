#pragma once

#include <cstdint>
#include <expected>

#include "host/ui/lvgl/square_common/system_page_layout.hpp"
#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// Owns the persistent quick-settings layer, its native LVGL controls, and the
// Guest-foreground performance HUD. Callers hold the LVGL adapter
// lock for methods whose name ends in Locked.
class StatusLayerUi final {
   public:
    explicit StatusLayerUi(const SystemPageLayout& layout) : cellular_layout_(layout) {}
    StatusLayerUi(const StatusLayerUi&) = delete;
    StatusLayerUi& operator=(const StatusLayerUi&) = delete;
    ~StatusLayerUi();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(const host_ui::StatusLayerModel& model,
                                                                         host_ui::SystemUiActionSink action_sink,
                                                                         void* action_context);
    void UpdateLocked(const host_ui::StatusLayerModel& model);
    void SetTransitionProgressLocked(uint16_t progress_per_mille);
    void Deactivate();
    void LeaveLocked();
    [[nodiscard]] void* ActionContext() const;
    [[nodiscard]] bool CellularSettingsPage() const { return cellular_settings_page_; }
    [[nodiscard]] lv_obj_t* TransitionDialogLocked() const {
        return cellular_dialog_ != nullptr && !lv_obj_has_flag(cellular_dialog_, LV_OBJ_FLAG_HIDDEN) ? cellular_dialog_
                                                                                                     : status_dialog_;
    }
    [[nodiscard]] int32_t TransitionDialogVisibleY() const;
    [[nodiscard]] int32_t TransitionDialogHiddenY() const;
    [[nodiscard]] uint32_t TransitionScrimRgb() const;
    [[nodiscard]] uint8_t TransitionScrimOpacity() const;

    void UpdatePerformanceOverlayLocked(bool enabled, const CpuUsageSample& cpu,
                                        uint32_t guest_presented_frame_sequence);
    void RaisePerformanceOverlayLocked();
    [[nodiscard]] bool PerformanceOverlayVisibleLocked() const;

    // Off-screen rendering of the performance HUD for Direct Surface scanout:
    // the LVGL object stays hidden (so the Guest keeps the panel) and its
    // ARGB8888 snapshot is handed to the presenter instead. The pixels stay
    // valid until the next call. `fps` is measured by the caller.
    struct PerformanceOverlaySnapshot final {
        const uint8_t* pixels{};
        uint32_t width{};
        uint32_t height{};
        uint32_t stride{};
        int32_t x{};
        int32_t y{};
    };
    [[nodiscard]] bool RenderPerformanceOverlaySnapshotLocked(const CpuUsageSample& cpu, uint32_t fps,
                                                              PerformanceOverlaySnapshot& snapshot_out);

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
        int32_t performance_overlay_y{};
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
    static void CellularBackEvent(lv_event_t* event);
    static void CellularScrollEvent(lv_event_t* event);
    static void CellularEvent(lv_event_t* event);
    void ShowCellularDialogLocked();
    void UpdateCellularDialogLocked();
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
    bool cellular_settings_page_{};
    bool cellular_available_{};
    bool cellular_enabled_{};
    bool cellular_switching_{};
    bool cellular_sim_pending_{};
    bool cellular_sim_failed_{};
    uint8_t cellular_sim_restart_seconds_{};
    bool cellular_switch_failed_{};
    device::CellularSimSlot cellular_sim_slot_{device::CellularSimSlot::kUnknown};
    lv_obj_t* cellular_dialog_{};
    lv_obj_t* cellular_status_{};
    lv_obj_t* cellular_mode_{};
    lv_obj_t* cellular_mode_label_{};
    lv_obj_t* cellular_sim_buttons_[2]{};
    lv_obj_t* cellular_refresh_{};
    const SystemPageLayout& cellular_layout_;
    device::CellularDiagnostics cellular_diagnostics_{};
    device::CellularState cellular_state_{};
    bool cellular_connected_{};
    lv_obj_t* cellular_hint_{};
    lv_obj_t* cellular_freshness_{};
    lv_obj_t* cellular_sim_labels_[2]{};
    lv_obj_t* cellular_detail_values_[8]{};
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
    // ARGB8888 snapshot of the hidden HUD for Direct Surface scanout.
    uint8_t* performance_snapshot_pixels_{};
    uint32_t performance_snapshot_capacity_{};
    bool updating_controls_{};
    const Layout* layout_{};

    void EnsurePerformanceOverlayLocked();
    void SetPerformanceOverlayTextLocked(const CpuUsageSample& cpu, uint32_t fps);
};

}  // namespace micropixel::host_ui::lvgl::square_common
