#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

namespace micropixel::platform::audio {
class AudioEngine;
}

namespace micropixel::platform::esp32_s3_box_3 {

// The side mute switch is a Host control. Hardware gates the amplifier and
// this monitor also zeros the Host mix without exposing a Guest key.
class HostMuteControl final {
   public:
    ~HostMuteControl();

    [[nodiscard]] esp_err_t Initialize(audio::AudioEngine& audio);

   private:
    static void OnEdge(void* context);
    void ApplyLevel() const;

    audio::AudioEngine* audio_{};
    bool isr_registered_{};
};

}  // namespace micropixel::platform::esp32_s3_box_3
