// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/presentation.hpp"

#include <algorithm>

#include "esp_log.h"
#include "platform/audio/audio_engine.hpp"
#include "platform/boards/m5stack-tab5/board_hardware.hpp"
#include "platform/boards/m5stack-tab5/display_hardware.hpp"
#include "platform/boards/m5stack-tab5/tab5_state.hpp"

namespace micropixel::platform::m5stack_tab5 {

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> Tab5Presentation::CaptureScreenJpeg() {
    return CaptureDisplayedFrame(state_.display, state_.panel, static_cast<uint32_t>(kWidth),
                                 static_cast<uint32_t>(kHeight));
}

void Tab5Presentation::ApplyBrightness(uint8_t percent) {
    const esp_err_t status = hardware_.SetBrightness(std::min<uint8_t>(percent, 100U));
    if (status != ESP_OK) {
        ESP_LOGW(log_tag_, "brightness update failed: %s", esp_err_to_name(status));
    }
}

void Tab5Presentation::ApplyVolume(uint8_t percent) {
    if (audio_ != nullptr) {
        audio_->SetMasterVolumePercent(percent);
    }
}

}  // namespace micropixel::platform::m5stack_tab5
