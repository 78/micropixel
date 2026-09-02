#pragma once

#include "esp_err.h"
#include "host/ui/lvgl/square_common/square_presentation.hpp"

namespace micropixel::platform::audio {
class AudioEngine;
}

namespace micropixel::platform::esp32_s3_common {

struct Landscape320State;

class Landscape320Presentation final : public host_ui::lvgl::square_common::SquarePresentation,
                                       public host_ui::lvgl::square_common::ScreenCapture,
                                       public host_ui::lvgl::square_common::BrightnessControl,
                                       public host_ui::lvgl::square_common::VolumeControl {
   public:
    using BrightnessSetter = esp_err_t (*)(void* context, int percent);

    Landscape320Presentation(Landscape320State& state, const char* log_tag, BrightnessSetter brightness_setter,
                             void* brightness_context)
        : state_(state),
          log_tag_(log_tag),
          brightness_setter_(brightness_setter),
          brightness_context_(brightness_context) {}
    void BindAudioEngine(audio::AudioEngine& audio) { audio_ = &audio; }

    [[nodiscard]] host_ui::lvgl::square_common::ScreenCapture* Capture() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::BrightnessControl* Brightness() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::VolumeControl* Volume() override { return this; }

    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override;
    void ApplyBrightness(uint8_t percent) override;
    void ApplyVolume(uint8_t percent) override;

   private:
    Landscape320State& state_;
    const char* log_tag_{};
    BrightnessSetter brightness_setter_{};
    void* brightness_context_{};
    audio::AudioEngine* audio_{};
};

}  // namespace micropixel::platform::esp32_s3_common
