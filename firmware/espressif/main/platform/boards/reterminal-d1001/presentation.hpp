// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/ui/lvgl/square_common/square_presentation.hpp"

namespace micropixel::platform::audio {
class AudioEngine;
}

namespace micropixel::platform::reterminal_d1001 {

class BoardHardware;
struct D1001State;

// Board presentation roles: brightness goes to the LEDC backlight, volume to
// the shared AudioEngine when audio is present, and screenshots read the
// displayed DPI framebuffer through the board display layer.
class D1001Presentation final : public host_ui::lvgl::square_common::SquarePresentation,
                                public host_ui::lvgl::square_common::ScreenCapture,
                                public host_ui::lvgl::square_common::BrightnessControl,
                                public host_ui::lvgl::square_common::VolumeControl {
   public:
    D1001Presentation(D1001State& state, BoardHardware& hardware, const char* log_tag)
        : state_(state), hardware_(hardware), log_tag_(log_tag) {}

    void BindAudioEngine(audio::AudioEngine& audio) { audio_ = &audio; }

    [[nodiscard]] host_ui::lvgl::square_common::ScreenCapture* Capture() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::BrightnessControl* Brightness() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::VolumeControl* Volume() override { return this; }
    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override;
    void ApplyBrightness(uint8_t percent) override;
    void ApplyVolume(uint8_t percent) override;

   private:
    D1001State& state_;
    BoardHardware& hardware_;
    const char* log_tag_{};
    audio::AudioEngine* audio_{};
};

}  // namespace micropixel::platform::reterminal_d1001