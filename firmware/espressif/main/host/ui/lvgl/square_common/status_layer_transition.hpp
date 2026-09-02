#pragma once

#include <cstdint>
#include <expected>

#include "host/ui/lvgl/square_common/status_layer_ui.hpp"
#include "host/ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::host_ui::lvgl::square_common {

class StatusLayerTransition {
   public:
    virtual ~StatusLayerTransition() = default;

    [[nodiscard]] virtual bool BeginStatusLayerTransition(bool entering, uint32_t scrim_rgb, uint8_t scrim_opacity,
                                                          uint64_t trigger_timestamp_us) = 0;
    [[nodiscard]] virtual bool AnimateStatusLayerLocked(lv_obj_t* dialog, int32_t visible_y, int32_t hidden_y,
                                                        bool entering, uint32_t duration_ms,
                                                        uint64_t trigger_timestamp_us) = 0;
    [[nodiscard]] virtual bool FinishStatusLayerTransition(bool keep_buffers) = 0;
    virtual void CancelStatusLayerTransition() = 0;
};

// Boards without a display compositor still use the complete status-layer UI,
// but present it as a static LVGL frame instead of allocating transition
// buffers. This keeps the fallback explicit at the presentation boundary.
class StaticStatusLayerTransition final : public StatusLayerTransition {
   public:
    [[nodiscard]] bool BeginStatusLayerTransition(bool, uint32_t, uint8_t, uint64_t) override { return false; }
    [[nodiscard]] bool AnimateStatusLayerLocked(lv_obj_t*, int32_t, int32_t, bool, uint32_t, uint64_t) override {
        return false;
    }
    [[nodiscard]] bool FinishStatusLayerTransition(bool) override { return false; }
    void CancelStatusLayerTransition() override {}
};

struct StatusLayerPresentation final {
    bool hardware_accelerated{};
    uint32_t elapsed_us{};
};

[[nodiscard]] std::expected<StatusLayerPresentation, host_ui::SystemUiError> PresentStatusLayer(
    lv_display_t* display, StatusLayerUi& ui, StatusLayerTransition& transition, const host_ui::StatusLayerModel& model,
    uint64_t trigger_timestamp_us, host_ui::SystemUiActionSink action_sink, void* action_context,
    bool allow_software_animation);

[[nodiscard]] StatusLayerPresentation DismissStatusLayer(lv_display_t* display, StatusLayerUi& ui,
                                                         StatusLayerTransition& transition,
                                                         uint64_t trigger_timestamp_us, bool allow_software_animation);

}  // namespace micropixel::host_ui::lvgl::square_common
