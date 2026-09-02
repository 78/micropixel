#include "platform/boards/esp32-s3-common/presentation.hpp"

#include <algorithm>

#include "esp_log.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/esp32-s3-common/landscape_320_state.hpp"
#include "platform/lvgl/display/screen_capture.hpp"

namespace micropixel::platform::esp32_s3_common {

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> Landscape320Presentation::CaptureScreenJpeg() {
    return lvgl::CaptureScreenJpeg(state_.display, static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight),
                                   {.pixels = state_.display_shadow.Pixels(),
                                    .stride = state_.display_shadow.Stride(),
                                    .format = lvgl::DisplayCapturePixelFormat::kRgb565,
                                    .ready = state_.display_shadow.Ready()});
}

void Landscape320Presentation::ApplyBrightness(uint8_t percent) {
    const esp_err_t status = brightness_setter_ != nullptr
                                 ? brightness_setter_(brightness_context_, std::min<uint8_t>(percent, 100U))
                                 : ESP_ERR_NOT_SUPPORTED;
    if (status != ESP_OK) {
        ESP_LOGW(log_tag_, "brightness update failed: %s", esp_err_to_name(status));
    }
}

void Landscape320Presentation::ApplyVolume(uint8_t percent) {
    if (audio_ != nullptr) {
        audio_->SetMasterVolumePercent(percent);
    }
}

}  // namespace micropixel::platform::esp32_s3_common
