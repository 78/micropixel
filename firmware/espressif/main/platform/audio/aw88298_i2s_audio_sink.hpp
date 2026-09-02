#pragma once

#include "platform/audio/i2s_codec_audio_sink.hpp"

namespace micropixel::platform::audio {

struct Aw88298CodecConfig final {
    int pa_gain{15};
};

class Aw88298I2sAudioSink final : public I2sCodecAudioSink {
   public:
    Aw88298I2sAudioSink(I2sCodecAudioConfig common, Aw88298CodecConfig codec)
        : I2sCodecAudioSink(common), codec_(codec) {}

   protected:
    [[nodiscard]] const audio_codec_if_t* CreateCodec(const audio_codec_ctrl_if_t* control,
                                                      const audio_codec_gpio_if_t* gpio) override;

   private:
    Aw88298CodecConfig codec_{};
};

}  // namespace micropixel::platform::audio
