#include "platform/lvgl/ui/square_common/status_layer_transition.hpp"

#include <algorithm>

#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "src/core/lv_refr_private.h"

namespace micropixel::platform::lvgl::square_common {
namespace {

constexpr uint32_t kTransitionDurationMs = 100U;
constexpr uint32_t kTransitionFrameCount = 6U;
constexpr uint16_t kTransitionComplete = 1000U;

uint16_t EaseInCubic(uint16_t progress) {
    const uint64_t value = progress;
    return static_cast<uint16_t>(value * value * value / 1000000U);
}

uint16_t EaseOutCubic(uint16_t progress) {
    return static_cast<uint16_t>(kTransitionComplete - EaseInCubic(kTransitionComplete - progress));
}

void RunSoftwareTransition(lv_display_t* display, StatusLayerUi& ui, bool entering, bool animate) {
    if (!animate) {
        if (display != nullptr && esp_lv_adapter_lock(-1) == ESP_OK) {
            ui.SetTransitionProgressLocked(entering ? kTransitionComplete : 0U);
            lv_refr_now(display);
            esp_lv_adapter_unlock();
        }
        return;
    }
    const TickType_t frame_period =
        std::max<TickType_t>(1U, pdMS_TO_TICKS(kTransitionDurationMs / kTransitionFrameCount));
    TickType_t next_frame = xTaskGetTickCount();
    for (uint32_t frame = 0U; frame <= kTransitionFrameCount; ++frame) {
        const uint16_t linear = static_cast<uint16_t>(frame * kTransitionComplete / kTransitionFrameCount);
        const uint16_t progress = entering ? EaseOutCubic(linear) : kTransitionComplete - EaseInCubic(linear);
        if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
            break;
        }
        ui.SetTransitionProgressLocked(progress);
        RequestDisplayRefresh(display);
        esp_lv_adapter_unlock();
        if (frame != kTransitionFrameCount) {
            vTaskDelayUntil(&next_frame, frame_period);
        }
    }
}

}  // namespace

std::expected<StatusLayerPresentation, host_ui::SystemUiError> PresentStatusLayer(
    lv_display_t* display, StatusLayerUi& ui, StatusLayerTransitionBackend& transition,
    const host_ui::StatusLayerModel& model, uint64_t trigger_timestamp_us, host_ui::SystemUiActionSink action_sink,
    void* action_context, bool allow_software_animation) {
    const int64_t started_us = esp_timer_get_time();
    const bool hardware_started = transition.BeginStatusLayerTransition(
        true, ui.TransitionScrimRgb(), ui.TransitionScrimOpacity(), trigger_timestamp_us);
    if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        if (hardware_started) {
            transition.CancelStatusLayerTransition();
        }
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    auto result = ui.ShowLocked(model, action_sink, action_context);
    bool hardware_finished = false;
    if (result.has_value() && hardware_started) {
        const bool animated = transition.AnimateStatusLayerLocked(
            ui.TransitionDialogLocked(), ui.TransitionDialogVisibleY(), ui.TransitionDialogHiddenY(), true,
            kTransitionDurationMs, trigger_timestamp_us);
        if (animated) {
            ui.SetTransitionProgressLocked(kTransitionComplete);
            hardware_finished = transition.FinishStatusLayerTransition(true);
        }
        if (!animated || !hardware_finished) {
            transition.CancelStatusLayerTransition();
        }
    }
    esp_lv_adapter_unlock();
    if (!result.has_value()) {
        if (hardware_started) {
            transition.CancelStatusLayerTransition();
        }
        return std::unexpected(result.error());
    }
    if (!hardware_finished) {
        RunSoftwareTransition(display, ui, true, allow_software_animation);
    }
    return StatusLayerPresentation{
        .hardware_accelerated = hardware_finished,
        .elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us),
    };
}

StatusLayerPresentation DismissStatusLayer(lv_display_t* display, StatusLayerUi& ui,
                                           StatusLayerTransitionBackend& transition, uint64_t trigger_timestamp_us,
                                           bool allow_software_animation) {
    const int64_t started_us = esp_timer_get_time();
    ui.Deactivate();
    const bool hardware_started = transition.BeginStatusLayerTransition(
        false, ui.TransitionScrimRgb(), ui.TransitionScrimOpacity(), trigger_timestamp_us);
    if (display == nullptr || esp_lv_adapter_lock(-1) != ESP_OK) {
        if (hardware_started) {
            transition.CancelStatusLayerTransition();
        }
        return {.hardware_accelerated = false, .elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us)};
    }
    if (hardware_started) {
        const bool animated = transition.AnimateStatusLayerLocked(
            ui.TransitionDialogLocked(), ui.TransitionDialogVisibleY(), ui.TransitionDialogHiddenY(), false,
            kTransitionDurationMs, trigger_timestamp_us);
        if (animated) {
            ui.LeaveLocked();
            // The compositor's final frame has already restored the retained
            // Hall/Guest background. Removing the hidden LVGL overlay still
            // invalidates its old full-screen bounds; discard that damage
            // while the LVGL lock and dummy draw mode are both held so it
            // cannot trigger a redundant software redraw after the hardware
            // transition hands the panel back.
            (void)lv_inv_area(display, nullptr);
            const bool finished = transition.FinishStatusLayerTransition(false);
            if (!finished) {
                transition.CancelStatusLayerTransition();
            }
            esp_lv_adapter_unlock();
            return {.hardware_accelerated = finished,
                    .elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us)};
        }
        transition.CancelStatusLayerTransition();
    }
    esp_lv_adapter_unlock();
    RunSoftwareTransition(display, ui, false, allow_software_animation);
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ui.LeaveLocked();
        if (!allow_software_animation) {
            lv_refr_now(display);
        }
        esp_lv_adapter_unlock();
    }
    return {.hardware_accelerated = false, .elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - started_us)};
}

}  // namespace micropixel::platform::lvgl::square_common
