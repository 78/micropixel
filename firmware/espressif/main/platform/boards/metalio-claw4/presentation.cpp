#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "device/contracts/graphics.hpp"
#include "draw/lv_draw_buf_private.h"
#include "esp_async_color_convert.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
#include "src/draw/snapshot/lv_snapshot.h"
#endif
#include "host/ui/lvgl/square_common/hall_cover_codec.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/metalio-claw4/battery_peripheral.hpp"
#include "platform/boards/metalio-claw4/board_config.hpp"
#include "platform/boards/metalio-claw4/board_io.hpp"
#include "platform/boards/metalio-claw4/display/display_pipeline.hpp"
#include "platform/boards/metalio-claw4/display/screen_capture.hpp"
#include "platform/boards/metalio-claw4/gpio_peripheral.hpp"
#include "platform/boards/metalio-claw4/platform_state.hpp"
#include "platform/boards/metalio-claw4/presentation.hpp"
#include "platform/boards/metalio-claw4/sensor_peripheral.hpp"
#include "platform/boards/metalio-claw4/tca9555_power_key.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/input/gt911_input.hpp"
#include "platform/lvgl/display/jpeg_cover_decoder.hpp"
#include "platform/lvgl/display/png_cover_decoder.hpp"
#include "platform/lvgl/display/system_transition_compositor.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_pointer_router.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/platform.hpp"
#include "platform/random/system_random.hpp"
#include "platform/wifi/esp_hosted_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "work/background_executor.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform::metalio_claw4::detail {

namespace {

void MaskHallCoverCorners(const host_ui::lvgl::square_common::HallTransitionPresentation& presentation,
                          uint8_t* destination) {
    // Bake the four small corner masks into the retained bitmap instead of
    // making LVGL allocate a card-sized rounded clipping layer on every redraw.
    // The cover is drawn full-bleed below the card's post-drawn border, so its
    // baked corner mask must use the same outer radius as the card.
    host_ui::lvgl::square_common::MaskHallTransitionCoverRgb888(presentation, destination);
}

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrameImpl(
    MetalioClaw4BoardState& state, const host_ui::lvgl::square_common::HallTransitionPresentation& presentation,
    uint64_t trigger_timestamp_us, uint32_t transition_duration_ms) {
#if CONFIG_MICROPIXEL_SYSTEM_SHELL_SNAPSHOT
    lv_obj_t* guest_frame = state.guest_graphics.FrameLocked();
    if (state.display == nullptr || guest_frame == nullptr || state.guest_snapshot_pixels != nullptr ||
        state.guest_transition_pixels != nullptr || !presentation.card_valid) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    // The controller hides final-display Host overlays before capture. Flush
    // that invalidated region now so the direct framebuffer used as the
    // transition source contains only Guest pixels.
    const esp_err_t overlay_refresh_status = esp_lv_adapter_refresh_now(state.display);
    if (overlay_refresh_status != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    const uint32_t thumbnail_allocation_bytes =
        (presentation.cover_bytes + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
    const uint32_t intermediate_bytes = presentation.intermediate_size * presentation.intermediate_size * 3U;
    const uint32_t intermediate_allocation_bytes =
        (intermediate_bytes + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
    auto* thumbnail = static_cast<uint8_t*>(heap_caps_aligned_calloc(kPpaBufferAlignment, thumbnail_allocation_bytes,
                                                                     1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (thumbnail == nullptr) {
        ESP_LOGE(kTag, "suspended-App thumbnail could not allocate %" PRIu32 " PSRAM bytes",
                 thumbnail_allocation_bytes);
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }

    lv_draw_buf_t thumbnail_draw_buf{};
    if (lv_draw_buf_init(&thumbnail_draw_buf, presentation.cover_size, presentation.cover_size, LV_COLOR_FORMAT_RGB888,
                         presentation.cover_stride, thumbnail, presentation.cover_bytes) != LV_RESULT_OK) {
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "could not initialize suspended-App thumbnail draw buffer");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    auto* half = static_cast<uint8_t*>(heap_caps_aligned_alloc(kPpaBufferAlignment, intermediate_allocation_bytes,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (half == nullptr) {
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "suspended-App transition source could not allocate %" PRIu32 " PSRAM bytes",
                 intermediate_allocation_bytes);
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    if (!state.system_transition.ResetBackgroundToBaseline()) {
        heap_caps_free(half);
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "could not restore baseline Hall background before transition");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    uint32_t half_elapsed_us = 0U;
    const bool scaled_half =
        state.system_transition.CaptureDisplayedToHalf(half, intermediate_allocation_bytes,
                                                       lvgl::SystemTransitionRect{.x = presentation.card.x,
                                                                                  .y = presentation.card.y,
                                                                                  .width = presentation.card.width,
                                                                                  .height = presentation.card.width},
                                                       trigger_timestamp_us, half_elapsed_us);
    const int64_t scale_started_us = esp_timer_get_time();
    const bool scaled_card =
        scaled_half && state.system_transition.ScaleHalfToCard(half, thumbnail, thumbnail_allocation_bytes);
    const uint32_t card_elapsed_us = static_cast<uint32_t>(esp_timer_get_time() - scale_started_us);
    if (!scaled_card) {
        state.system_transition.CancelPreparedToHall();
        heap_caps_free(half);
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "PPA suspended-App transition preparation failed");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    MaskHallCoverCorners(presentation, thumbnail);
    lv_draw_buf_flush_cache(&thumbnail_draw_buf, nullptr);

    const lvgl::SystemTransitionRect cover_region{.x = presentation.card.x,
                                                  .y = presentation.card.y,
                                                  .width = presentation.card.width,
                                                  .height = presentation.card.width};
    const bool background_patched = state.system_transition.UpdateBackgroundPixels(thumbnail, cover_region);
    const bool animated = background_patched &&
                          state.system_transition.Animate(half, cover_region, lvgl::SystemTransitionDirection::kToHall,
                                                          transition_duration_ms, trigger_timestamp_us);
    heap_caps_free(half);
    if (!animated) {
        state.system_transition.CancelPreparedToHall();
        heap_caps_free(thumbnail);
        ESP_LOGE(kTag, "PPA suspended-App early Hall transition failed");
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }

    state.guest_snapshot_pixels = thumbnail;
    state.guest_transition_pixels = nullptr;
    state.guest_snapshot_in_hall = true;
    ESP_LOGI(kTag,
             "captured suspended-App snapshot: source=displayed-%dx%d RGB888 thumbnail=%" PRIu32 "x%" PRIu32
             " bytes=%" PRIu32 " first-frame=%" PRIu32 " us ppa-card=%" PRIu32 " us transition=complete",
             kWidth, kHeight, presentation.cover_size, presentation.cover_size, presentation.cover_bytes,
             half_elapsed_us, card_elapsed_us);
    return host_ui::HallCoverModel{.data = thumbnail,
                                   .size = presentation.cover_bytes,
                                   .width = presentation.cover_size,
                                   .height = presentation.cover_size,
                                   .stride = presentation.cover_stride};
#else
    (void)state;
    (void)presentation;
    (void)trigger_timestamp_us;
    (void)transition_duration_ms;
    return std::unexpected(host_ui::SystemUiError::kUnavailable);
#endif
}

void ReleaseGuestSnapshotImpl(MetalioClaw4BoardState& state) {
    state.system_transition.CancelPreparedToHall();
    if (state.guest_snapshot_pixels == nullptr && state.guest_transition_pixels == nullptr) {
        return;
    }
    heap_caps_free(state.guest_snapshot_pixels);
    heap_caps_free(state.guest_transition_pixels);
    state.guest_snapshot_pixels = nullptr;
    state.guest_transition_pixels = nullptr;
    state.guest_snapshot_in_hall = false;
    ESP_LOGI(kTag, "released suspended-App snapshot");
}

bool AnimateToGuestImpl(MetalioClaw4BoardState& state, lv_obj_t* hall_root, lv_obj_t* guest_frame,
                        const host_ui::lvgl::square_common::HallTransitionPresentation& presentation,
                        uint32_t transition_duration_ms) {
    if (state.display == nullptr || hall_root == nullptr || guest_frame == nullptr ||
        !state.guest_graphics.RefreshSynchronizationAvailable()) {
        return false;
    }

    const uint32_t intermediate_bytes = presentation.intermediate_size * presentation.intermediate_size * 3U;
    const uint32_t intermediate_allocation_bytes =
        (intermediate_bytes + kPpaBufferAlignment - 1U) / kPpaBufferAlignment * kPpaBufferAlignment;
    auto* transition_fullscreen =
        static_cast<uint8_t*>(heap_caps_aligned_alloc(64U, kGuestTransitionBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* transition_half = static_cast<uint8_t*>(heap_caps_aligned_alloc(
        kPpaBufferAlignment, intermediate_allocation_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    lv_draw_buf_t transition_snapshot{};
    bool transition_ready =
        transition_fullscreen != nullptr && transition_half != nullptr && presentation.card_valid &&
        state.guest_snapshot_in_hall &&
        lv_draw_buf_init(&transition_snapshot, kWidth, kHeight, LV_COLOR_FORMAT_RGB888, kGuestTransitionStride,
                         transition_fullscreen, kGuestTransitionBytes) == LV_RESULT_OK;
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        heap_caps_free(transition_half);
        heap_caps_free(transition_fullscreen);
        return false;
    }
    state.guest_graphics.DrainRefreshReady();
    if (transition_ready) {
        transition_ready =
            lv_snapshot_take_to_draw_buf(guest_frame, LV_COLOR_FORMAT_RGB888, &transition_snapshot) == LV_RESULT_OK;
        if (transition_ready) {
            lv_draw_buf_flush_cache(&transition_snapshot, nullptr);
            const int64_t background_started_us = esp_timer_get_time();
            transition_ready = state.system_transition.RefreshBackgroundLocked(hall_root);
            if (transition_ready) {
                ESP_LOGI(kTag, "Hall transition background refreshed before App resume: elapsed=%" PRIu32 " us",
                         static_cast<uint32_t>(esp_timer_get_time() - background_started_us));
            }
        }
    }
    esp_lv_adapter_unlock();

    if (transition_ready) {
        transition_ready = state.system_transition.ScaleFullscreenToHalf(transition_fullscreen, transition_half,
                                                                         intermediate_allocation_bytes);
    }
    heap_caps_free(transition_fullscreen);
    if (transition_ready) {
        transition_ready =
            state.system_transition.Animate(transition_half,
                                            lvgl::SystemTransitionRect{.x = presentation.card.x,
                                                                       .y = presentation.card.y,
                                                                       .width = presentation.card.width,
                                                                       .height = presentation.card.width},
                                            lvgl::SystemTransitionDirection::kToGuest, transition_duration_ms);
    } else {
        state.system_transition.ClearBackground();
    }
    heap_caps_free(transition_half);
    state.guest_snapshot_in_hall = false;

    ESP_LOGI(kTag, "retained Guest view restored from App Hall: transition=%s", transition_ready ? "PPA" : "direct");
    return transition_ready;
}

}  // namespace

void MetalioClaw4Presentation::BeforeHallRebuildLocked() { state_.touch_input.ClearSmokeUi(); }

bool MetalioClaw4Presentation::RetainNativeCover(const host_ui::HallCoverModel& source,
                                                 host_ui::HallCoverModel& prepared) {
    if (source.format != host_ui::HallCoverFormat::kRgb888 || source.data != state_.guest_snapshot_pixels ||
        source.width == 0U || source.height != source.width ||
        source.stride != host_ui::lvgl::square_common::HallCoverStride(source.width) ||
        source.size != source.stride * source.height) {
        return false;
    }
    prepared = source;
    return true;
}

bool MetalioClaw4Presentation::EnterTransitionPending() const {
    return state_.guest_transition_pixels != nullptr && !state_.guest_snapshot_in_hall;
}

bool MetalioClaw4Presentation::BackgroundAvailable() const { return state_.system_transition.HasBackground(); }

bool MetalioClaw4Presentation::PrepareBackgroundLocked(lv_obj_t* root) {
    return state_.system_transition.PrepareBackgroundLocked(root);
}

bool MetalioClaw4Presentation::UpdateBackgroundRegionLocked(lv_obj_t* object,
                                                            host_ui::lvgl::square_common::HallPresentationRect rect) {
    return state_.system_transition.UpdateBackgroundRegionLocked(
        object, {.x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height});
}

bool MetalioClaw4Presentation::AnimateToHall(host_ui::lvgl::square_common::HallPresentationRect rect,
                                             uint32_t duration_ms, uint64_t trigger_timestamp_us) {
    const bool animated = state_.system_transition.Animate(
        state_.guest_transition_pixels, {.x = rect.x, .y = rect.y, .width = rect.width, .height = rect.width},
        lvgl::SystemTransitionDirection::kToHall, duration_ms, trigger_timestamp_us);
    heap_caps_free(state_.guest_transition_pixels);
    state_.guest_transition_pixels = nullptr;
    state_.guest_snapshot_in_hall = true;
    return animated;
}

void MetalioClaw4Presentation::CancelEnterTransition() {
    if (state_.guest_transition_pixels == nullptr) {
        return;
    }
    state_.system_transition.CancelPreparedToHall();
    heap_caps_free(state_.guest_transition_pixels);
    state_.guest_transition_pixels = nullptr;
    state_.guest_snapshot_in_hall = true;
    state_.system_transition.ClearBackground();
}

bool MetalioClaw4Presentation::AnimateToGuest(
    lv_obj_t* hall_root, lv_obj_t* guest_frame,
    const host_ui::lvgl::square_common::HallTransitionPresentation& presentation, uint32_t duration_ms) {
    return AnimateToGuestImpl(state_, hall_root, guest_frame, presentation, duration_ms);
}

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> MetalioClaw4Presentation::CaptureGuestFrame(
    const host_ui::lvgl::square_common::HallTransitionPresentation& presentation, uint64_t trigger_timestamp_us,
    uint32_t duration_ms) {
    return CaptureGuestFrameImpl(state_, presentation, trigger_timestamp_us, duration_ms);
}

void MetalioClaw4Presentation::ReleaseGuestSnapshot() { ReleaseGuestSnapshotImpl(state_); }

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> MetalioClaw4Presentation::CaptureScreenJpeg() {
    return metalio_claw4::CaptureScreenJpeg(state_.display, state_.board_io.Panel(), kWidth, kHeight);
}

void MetalioClaw4Presentation::ApplyBrightness(uint8_t percent) {
    const uint8_t safe_percent = percent <= 100U ? percent : 100U;
    const uint32_t output = controllers::BrightnessOutputPerTenThousand(safe_percent);
    const esp_err_t status = state_.display_pipeline.SetBrightness(output);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not set backlight brightness: %s", esp_err_to_name(status));
    }
}

void MetalioClaw4Presentation::ApplyVolume(uint8_t percent) {
    if (state_.audio_engine != nullptr) {
        state_.audio_engine->SetMasterVolumePercent(percent);
    }
}

void MetalioClaw4Presentation::PrepareShutdownLocked() { state_.touch_input.ClearSmokeUi(); }

}  // namespace micropixel::platform::metalio_claw4::detail
