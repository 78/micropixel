#include "platform/audio/aw88298_i2s_audio_sink.hpp"

#include "esp_codec_dev_defaults.h"

namespace micropixel::platform::audio {

const audio_codec_if_t* Aw88298I2sAudioSink::CreateCodec(const audio_codec_ctrl_if_t* control,
                                                         const audio_codec_gpio_if_t* gpio) {
    aw88298_codec_cfg_t config{};
    config.ctrl_if = control;
    config.gpio_if = gpio;
    config.hw_gain.pa_gain = codec_.pa_gain;
    return aw88298_codec_new(&config);
}

}  // namespace micropixel::platform::audio
