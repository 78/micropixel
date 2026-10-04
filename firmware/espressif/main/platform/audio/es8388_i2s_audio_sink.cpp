// SPDX-License-Identifier: Apache-2.0
#include "platform/audio/es8388_i2s_audio_sink.hpp"

#include "es8388_codec.h"

namespace micropixel::platform::audio {

const audio_codec_if_t* Es8388I2sAudioSink::CreateCodec(const audio_codec_ctrl_if_t* control,
                                                        const audio_codec_gpio_if_t* gpio) {
    es8388_codec_cfg_t config{};
    config.ctrl_if = control;
    config.gpio_if = gpio;
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    config.master_mode = false;
    config.pa_pin = static_cast<int16_t>(Config().amplifier_enable);
    config.pa_reverted = Config().amplifier_active_low;
    config.hw_gain.pa_voltage = codec_.amplifier_voltage;
    config.hw_gain.codec_dac_voltage = codec_.codec_dac_voltage;
    return es8388_codec_new(&config);
}

}  // namespace micropixel::platform::audio
