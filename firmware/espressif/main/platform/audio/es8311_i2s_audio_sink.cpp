#include "platform/audio/es8311_i2s_audio_sink.hpp"

#include "esp_codec_dev_defaults.h"

namespace micropixel::platform::audio {

const audio_codec_if_t* Es8311I2sAudioSink::CreateCodec(const audio_codec_ctrl_if_t* control,
                                                        const audio_codec_gpio_if_t* gpio) {
    es8311_codec_cfg_t config{};
    config.ctrl_if = control;
    config.gpio_if = gpio;
    config.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    config.pa_pin = Config().amplifier_enable;
    config.pa_reverted = Config().amplifier_active_low;
    config.master_mode = false;
    config.use_mclk = true;
    config.hw_gain.pa_voltage = codec_.amplifier_voltage;
    config.hw_gain.codec_dac_voltage = codec_.codec_dac_voltage;
    config.mclk_div = 256U;
    return es8311_codec_new(&config);
}

}  // namespace micropixel::platform::audio
