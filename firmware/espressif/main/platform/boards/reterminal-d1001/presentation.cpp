// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/reterminal-d1001/presentation.hpp"

#include <algorithm>

#include "esp_log.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/reterminal-d1001/board_hardware.hpp"
#include "platform/boards/reterminal-d1001/d1001_state.hpp"
#include "platform/boards/reterminal-d1001/display_hardware.hpp"

namespace micropixel::platform::reterminal_d1001 {

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> D1001Presentation::CaptureScreenJpeg() {
    return CaptureDisplayedFrame(state_.display, state_.panel, static_cast<uint32_t>(kWidth),
                                 static_cast<uint32_t>(kHeight));
}

void D1001Presentation::ApplyBrightness(uint8_t percent) {
    const esp_err_t status = hardware_.SetBrightness(std::min<uint8_t>(percent, 100U));
    if (status != ESP_OK) {
        ESP_LOGW(log_tag_, "brightness update failed: %s", esp_err_to_name(status));
    }
}

void D1001Presentation::ApplyVolume(uint8_t percent) {
    if (audio_ != nullptr) {
        audio_->SetMasterVolumePercent(percent);
    }
}

}  // namespace micropixel::platform::reterminal_d1001