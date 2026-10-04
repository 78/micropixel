// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "platform/audio/i2s_codec_audio_sink.hpp"

namespace micropixel::platform::audio {

struct Es8388CodecConfig final {
    float amplifier_voltage{5.0F};
    float codec_dac_voltage{3.3F};
};

// Register-controlled ES8388 (speaker DAC path). Boards that also route the
// ADC (microphone) keep it outside the Host audio contract.
class Es8388I2sAudioSink final : public I2sCodecAudioSink {
   public:
    Es8388I2sAudioSink(I2sCodecAudioConfig common, Es8388CodecConfig codec)
        : I2sCodecAudioSink(common), codec_(codec) {}

   protected:
    [[nodiscard]] const audio_codec_if_t* CreateCodec(const audio_codec_ctrl_if_t* control,
                                                      const audio_codec_gpio_if_t* gpio) override;

   private:
    Es8388CodecConfig codec_{};
};

}  // namespace micropixel::platform::audio
