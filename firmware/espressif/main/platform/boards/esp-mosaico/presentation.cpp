#include "platform/boards/esp-mosaico/presentation.hpp"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_async_color_convert.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ui/lvgl/square_common/hall_cover_codec.hpp"
#include "lvgl.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp-mosaico/battery_peripheral.hpp"
#include "platform/boards/esp-mosaico/board_config.hpp"
#include "platform/boards/esp-mosaico/display/panel_transition_compositor.hpp"
#include "platform/boards/esp-mosaico/platform_state.hpp"
#include "platform/boards/esp-mosaico/power_controller.hpp"
#include "platform/boards/esp-mosaico/usb_cdc_console.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/lvgl/display/system_transition_timeline.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_pointer_router.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/platform.hpp"
#include "platform/random/system_random.hpp"
#include "platform/transports/tinyusb_cdc_local_control.hpp"
#include "platform/wifi/native_wifi_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "src/display/lv_display_private.h"
#include "work/background_executor.hpp"

namespace micropixel::platform::esp_mosaico::detail {

bool MosaicoPresentation::AnimateToGuest(lv_obj_t* hall_root, lv_obj_t* guest_frame,
                                         const host_ui::lvgl::square_common::HallTransitionPresentation& presentation,
                                         uint32_t transition_duration_ms) {
    (void)hall_root;
    (void)guest_frame;
    (void)presentation;
    const bool transition_requested = state_.guest_transition_intermediate != nullptr &&
                                      state_.guest_snapshot_cover != nullptr && state_.guest_snapshot_in_hall;
    bool transitioned = false;
    if (transition_requested && *state_.display_pipeline.DisplayedShadowReady() &&
        state_.panel_transition.CaptureDisplayedBackground(state_.display_pipeline.DisplayedShadow())) {
        transitioned =
            state_.panel_transition.Animate(state_.guest_transition_intermediate, state_.guest_transition_card,
                                            PanelTransitionDirection::kToGuest, transition_duration_ms);
    }
    state_.guest_snapshot_in_hall = false;
    ESP_LOGI(kTag, "retained Guest view restored from App Hall: transition=%s", transitioned ? "PPA" : "direct");
    return transitioned;
}

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> MosaicoPresentation::CaptureGuestFrame(
    const host_ui::lvgl::square_common::HallTransitionPresentation& presentation, uint64_t trigger_timestamp_us,
    uint32_t transition_duration_ms) {
    lv_obj_t* guest_frame = state_.guest_graphics.FrameLocked();
    if (state_.display == nullptr || guest_frame == nullptr || !presentation.card_valid ||
        state_.guest_transition_intermediate != nullptr || state_.guest_snapshot_cover != nullptr ||
        state_.display_pipeline.DisplayedShadow() == nullptr || !*state_.display_pipeline.DisplayedShadowReady() ||
        !state_.panel_transition.HasBackground()) {
        ESP_LOGW(kTag, "retained Guest capture unavailable: shadow=%s Hall-baseline=%s",
                 *state_.display_pipeline.DisplayedShadowReady() ? "ready" : "incomplete",
                 state_.panel_transition.HasBackground() ? "ready" : "missing");
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    // The controller hides final-display Host overlays before capture.
    // Complete that small invalidated-area refresh synchronously so the
    // displayed shadow contains only Guest pixels when it is scaled.
    const esp_err_t overlay_refresh_status = esp_lv_adapter_refresh_now(state_.display);
    if (overlay_refresh_status != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    const uint32_t intermediate_bytes = presentation.intermediate_size * presentation.intermediate_size * 2U;
    const uint32_t intermediate_allocation_bytes =
        (intermediate_bytes + kTransitionAlignment - 1U) / kTransitionAlignment * kTransitionAlignment;
    const uint32_t cover_allocation_bytes =
        (presentation.cover_bytes + kTransitionAlignment - 1U) / kTransitionAlignment * kTransitionAlignment;
    auto* intermediate = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        kTransitionAlignment, intermediate_allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* cover = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kTransitionAlignment, cover_allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (intermediate == nullptr || cover == nullptr) {
        heap_caps_free(cover);
        heap_caps_free(intermediate);
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    state_.guest_transition_card = {.x = presentation.card.x,
                                    .y = presentation.card.y,
                                    .width = presentation.card.width,
                                    .height = presentation.card.height};

    uint32_t first_frame_elapsed_us = 0U;
    const bool first_frame_presented = state_.panel_transition.CaptureDisplayedToIntermediate(
        state_.display_pipeline.DisplayedShadow(), intermediate, intermediate_allocation_bytes,
        state_.guest_transition_card, trigger_timestamp_us, first_frame_elapsed_us);
    const int64_t cover_started_us = esp_timer_get_time();
    const bool cover_scaled = first_frame_presented && state_.panel_transition.ScaleIntermediateToCoverRgb888(
                                                           intermediate, cover, cover_allocation_bytes);
    if (cover_scaled) {
        host_ui::lvgl::square_common::MaskHallTransitionCoverRgb888(presentation, cover);
    }
    const bool background_patched =
        cover_scaled && state_.panel_transition.UpdateBackgroundRgb888(cover, state_.guest_transition_card);
    const uint32_t cover_elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - cover_started_us);
    const bool animated =
        background_patched &&
        state_.panel_transition.Animate(intermediate, state_.guest_transition_card, PanelTransitionDirection::kToHall,
                                        transition_duration_ms, trigger_timestamp_us);
    if (!animated) {
        state_.panel_transition.CancelPreparedToHall();
        heap_caps_free(intermediate);
        heap_caps_free(cover);
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    lv_draw_buf_t cover_draw_buf{};
    if (lv_draw_buf_init(&cover_draw_buf, presentation.cover_size, presentation.cover_size, LV_COLOR_FORMAT_RGB888,
                         presentation.cover_stride, cover, presentation.cover_bytes) == LV_RESULT_OK) {
        lv_draw_buf_flush_cache(&cover_draw_buf, nullptr);
    }
    state_.guest_transition_intermediate = intermediate;
    state_.guest_snapshot_cover = cover;
    state_.guest_snapshot_in_hall = true;
    ESP_LOGI(kTag,
             "captured retained Guest transition: source=displayed-%dx%d RGB565 intermediate=%" PRIu32 " cover=%" PRIu32
             "x%" PRIu32 " first-frame=%" PRIu32 " us cover=%" PRIu32 " us transition=complete",
             kWidth, kHeight, presentation.intermediate_size, presentation.cover_size, presentation.cover_size,
             first_frame_elapsed_us, cover_elapsed_us);
    return host_ui::HallCoverModel{
        .data = cover,
        .size = presentation.cover_bytes,
        .width = presentation.cover_size,
        .height = presentation.cover_size,
        .stride = presentation.cover_stride,
        .cache_key = reinterpret_cast<uintptr_t>(cover),
    };
}

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> MosaicoPresentation::CaptureScreenJpeg() {
    return lvgl::CaptureScreenJpeg(state_.display, static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight),
                                   {.pixels = state_.display_pipeline.DisplayedShadow(),
                                    .stride = kDisplayFrameStride,
                                    .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                    .ready = state_.display_pipeline.DisplayedShadowReady()});
}

void MosaicoPresentation::ReleaseGuestSnapshot() {
    state_.panel_transition.CancelPreparedToHall();
    heap_caps_free(state_.guest_snapshot_cover);
    heap_caps_free(state_.guest_transition_intermediate);
    state_.guest_snapshot_cover = nullptr;
    state_.guest_transition_intermediate = nullptr;
    state_.guest_transition_card = {};
    state_.guest_snapshot_in_hall = false;
}

bool MosaicoPresentation::PrepareHallBackgroundLocked() {
    const int64_t background_started_us = esp_timer_get_time();
    const bool prepared = *state_.display_pipeline.DisplayedShadowReady() &&
                          state_.panel_transition.CaptureDisplayedBackground(state_.display_pipeline.DisplayedShadow());
    if (prepared) {
        ESP_LOGI(kTag, "Hall transition baseline retained before App launch: elapsed=%" PRIu32 " us",
                 static_cast<uint32_t>(esp_timer_get_time() - background_started_us));
    } else {
        ESP_LOGW(kTag, "could not retain Hall transition baseline before App launch");
    }
    return prepared;
}

void MosaicoPresentation::ApplyBrightness(uint8_t percent) {
    const esp_err_t status = state_.display_pipeline.SetBrightness(static_cast<uint32_t>(percent) * 100U);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not set display brightness: %s", esp_err_to_name(status));
    }
}

void MosaicoPresentation::ApplyVolume(uint8_t percent) {
    if (audio_ != nullptr) {
        audio_->SetMasterVolumePercent(percent);
    }
}

bool MosaicoPresentation::RetainNativeCover(const host_ui::HallCoverModel& source, host_ui::HallCoverModel& prepared) {
    if (source.format != host_ui::HallCoverFormat::kRgb888 || source.data != state_.guest_snapshot_cover ||
        source.width == 0U || source.height != source.width ||
        source.stride != host_ui::lvgl::square_common::HallCoverStride(source.width) ||
        source.size != source.stride * source.height) {
        return false;
    }
    prepared = source;
    return true;
}

bool MosaicoPresentation::BackgroundAvailable() const { return state_.panel_transition.HasBackground(); }

bool MosaicoPresentation::PrepareBackgroundLocked(lv_obj_t*) { return PrepareHallBackgroundLocked(); }

bool MosaicoPresentation::UpdateBackgroundRegionLocked(lv_obj_t*, host_ui::lvgl::square_common::HallPresentationRect) {
    // CaptureGuestFrame already patches the retained Hall card region before
    // presenting the hardware transition.
    return state_.panel_transition.HasBackground();
}

bool MosaicoPresentation::AnimateToHall(host_ui::lvgl::square_common::HallPresentationRect, uint32_t, uint64_t) {
    // Mosaico presents the first frame while converting the panel shadow in
    // CaptureGuestFrame; no second animation pass is required.
    return state_.guest_snapshot_in_hall;
}

}  // namespace micropixel::platform::esp_mosaico::detail
