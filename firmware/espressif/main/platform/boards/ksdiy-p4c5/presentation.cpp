#include "platform/boards/ksdiy-p4c5/presentation.hpp"

#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/lvgl/display/screen_capture.hpp"

namespace micropixel::platform::ksdiy_p4c5::detail {
namespace {

// Returns the DPI framebuffer the panel is scanning out. Called with the LVGL
// lock held so LVGL cannot swap buffers underneath the caller.
const uint8_t* DisplayedFrameBuffer(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    constexpr uint32_t kFramebufferCount = 2U;
    void* panel_frame_buffers[kFramebufferCount]{};
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, kFramebufferCount, &panel_frame_buffers[0],
                                           &panel_frame_buffers[1]) != ESP_OK) {
        return nullptr;
    }
    // While a Direct Surface owns dummy draw, the adapter reports its free
    // buffer. Otherwise LVGL's active direct-mode buffer is the next
    // off-screen draw target, so the other one is on screen.
    auto* free_frame_buffer = static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display));
    if (free_frame_buffer == nullptr && lv_display_get_render_mode(display) == LV_DISPLAY_RENDER_MODE_DIRECT &&
        lv_display_is_double_buffered(display)) {
        const lv_draw_buf_t* active = lv_display_get_buf_active(display);
        if (active != nullptr) free_frame_buffer = static_cast<uint8_t*>(active->data);
    }
    if (free_frame_buffer == panel_frame_buffers[0]) {
        return static_cast<const uint8_t*>(panel_frame_buffers[1]);
    }
    if (free_frame_buffer == panel_frame_buffers[1]) {
        return static_cast<const uint8_t*>(panel_frame_buffers[0]);
    }
    return nullptr;
}

}  // namespace

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> KsdiyP4c5Presentation::CaptureScreenJpeg() {
    if (board::kLandscape || state_.display == nullptr || state_.board_io.Panel() == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    // The adapter lock is recursive; the shared capture copies the selected
    // framebuffer under the same lock before encoding it.
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    const uint8_t* displayed = DisplayedFrameBuffer(state_.display, state_.board_io.Panel());
    static constexpr bool kReady = true;
    auto result = displayed != nullptr ? lvgl::CaptureScreenJpeg(state_.display, kWidth, kHeight,
                                                                 {.pixels = displayed,
                                                                  .stride = static_cast<uint32_t>(kWidth) * 3U,
                                                                  .format = lvgl::DisplayCapturePixelFormat::kRgb888,
                                                                  .ready = &kReady})
                                       : std::expected<host_ui::ScreenCapture, host_ui::SystemUiError>(
                                             std::unexpected(host_ui::SystemUiError::kUnavailable));
    esp_lv_adapter_unlock();
    return result;
}

void KsdiyP4c5Presentation::ApplyBrightness(uint8_t percent) {
    const uint8_t safe_percent = percent <= 100U ? percent : 100U;
    const esp_err_t status =
        state_.display_pipeline.SetBrightness(controllers::BrightnessOutputPerTenThousand(safe_percent));
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not set backlight brightness: %s", esp_err_to_name(status));
    }
}

void KsdiyP4c5Presentation::ApplyVolume(uint8_t percent) {
    if (state_.audio_engine != nullptr) {
        state_.audio_engine->SetMasterVolumePercent(percent);
    }
}

}  // namespace micropixel::platform::ksdiy_p4c5::detail
