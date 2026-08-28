#pragma once

#include <cstdint>
#include <expected>

#include "host_ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/ui/square_common/status_layer_ui.hpp"

namespace micropixel::platform::lvgl::square_common {

class StatusLayerTransitionBackend {
   public:
    virtual ~StatusLayerTransitionBackend() = default;

    [[nodiscard]] virtual bool BeginStatusLayerTransition(bool entering, uint32_t scrim_rgb, uint8_t scrim_opacity,
                                                          uint64_t trigger_timestamp_us) = 0;
    [[nodiscard]] virtual bool AnimateStatusLayerLocked(lv_obj_t* dialog, int32_t visible_y, int32_t hidden_y,
                                                        bool entering, uint32_t duration_ms,
                                                        uint64_t trigger_timestamp_us) = 0;
    [[nodiscard]] virtual bool FinishStatusLayerTransition(bool keep_buffers) = 0;
    virtual void CancelStatusLayerTransition() = 0;
};

struct StatusLayerPresentation final {
    bool hardware_accelerated{};
    uint32_t elapsed_us{};
};

[[nodiscard]] std::expected<StatusLayerPresentation, host_ui::SystemUiError> PresentStatusLayer(
    lv_display_t* display, StatusLayerUi& ui, StatusLayerTransitionBackend& transition,
    const host_ui::StatusLayerModel& model, uint64_t trigger_timestamp_us, host_ui::SystemUiActionSink action_sink,
    void* action_context, bool allow_software_animation);

[[nodiscard]] StatusLayerPresentation DismissStatusLayer(lv_display_t* display, StatusLayerUi& ui,
                                                         StatusLayerTransitionBackend& transition,
                                                         uint64_t trigger_timestamp_us, bool allow_software_animation);

}  // namespace micropixel::platform::lvgl::square_common
