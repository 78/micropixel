// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "platform/audio/es8388_i2s_audio_sink.hpp"
#include "platform/boards/m5stack-tab5/board_config.hpp"
#include "platform/boards/m5stack-tab5/io_expander.hpp"

namespace micropixel::platform::m5stack_tab5::board {

// Speaker amplifier enable, driven through IO expander 1 P1. The shared sink
// calls this with enabled=false while it is muted and enables the amplifier
// only after its silent preroll, so neither the codec power-up nor a mute
// change is audible.
inline esp_err_t SetSpeakerAmplifier(void* context, bool enabled) {
    auto* expander = static_cast<Pi4ioeExpander*>(context);
    if (expander == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return expander->SetOutputBit(kSpeakerAmplifierUnit, kSpeakerAmplifierBit, enabled);
}

// ES8388 speaker path. The microphone side (ES7210) stays outside the Host
// audio contract for now. There is no speaker power-amplifier GPIO (pa_pin is
// -1 in the codec configuration), so the amplifier is driven through the IO
// expander setter above instead of a pin the codec driver would own.
inline audio::I2sCodecAudioConfig AudioConfig(Pi4ioeExpander& expander) {
    return {
        .name = "M5Stack Tab5 ES8388/I2S",
        .log_tag = "tab5_audio",
        .i2c_port = static_cast<int>(kSystemI2cPort),
        .i2s_port = I2S_NUM_0,
        .master_clock = kAudioMclk,
        .bit_clock = kAudioBitClock,
        .word_select = kAudioWordSelect,
        .data_out = kAudioDataOut,
        .amplifier_enable = GPIO_NUM_NC,
        .amplifier_setter = &SetSpeakerAmplifier,
        .amplifier_context = &expander,
        // ES8388 seven-bit address (the vendor BSP's 0x20 is the shifted form).
        .codec_i2c_address = 0x10U,
        .sample_rate = 16000U,
        .i2c_clock_hz = 100000U,
        .amplifier_preroll_ms = 24U,
        .dma_descriptor_count = 6U,
        .dma_frame_count = 240U,
        .output_channels = 2U,
        .amplifier_active_low = false,
        .probe_before_attach = true,
    };
}

inline audio::Es8388CodecConfig CodecConfig() { return {.amplifier_voltage = 5.0F, .codec_dac_voltage = 3.3F}; }

}  // namespace micropixel::platform::m5stack_tab5::board
