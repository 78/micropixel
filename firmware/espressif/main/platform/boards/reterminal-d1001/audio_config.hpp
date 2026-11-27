// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"
#include "platform/audio/es8311_i2s_audio_sink.hpp"
#include "platform/boards/reterminal-d1001/board_config.hpp"
#include "platform/boards/reterminal-d1001/board_hardware.hpp"

namespace micropixel::platform::reterminal_d1001::board {

// Speaker amplifier enable on PCA9535 P1.3. The shared sink calls this with
// enabled=false while muted and enables the amplifier only after its silent
// preroll, so the codec power-up transient is not audible.
inline esp_err_t SetSpeakerAmplifier(void* context, bool enabled) {
    auto* hardware = static_cast<BoardHardware*>(context);
    return hardware != nullptr ? hardware->SetSpeakerAmplifier(enabled) : ESP_ERR_INVALID_ARG;
}

inline audio::I2sCodecAudioConfig AudioConfig(BoardHardware& hardware) {
    return {
        .name = "reTerminal D1001 ES8311/I2S",
        .log_tag = "d1001_audio",
        .i2c_port = static_cast<int>(kSystemI2cPort),
        .i2s_port = I2S_NUM_0,
        .master_clock = kAudioMclk,
        .bit_clock = kAudioBitClock,
        .word_select = kAudioWordSelect,
        .data_out = kAudioDataOut,
        .amplifier_enable = GPIO_NUM_NC,
        .amplifier_setter = &SetSpeakerAmplifier,
        .amplifier_context = &hardware,
        .codec_i2c_address = kAudioCodecAddress,
        .sample_rate = kAudioSampleRate,
        .i2c_clock_hz = 100000U,
        .amplifier_preroll_ms = 24U,
        .dma_descriptor_count = 6U,
        .dma_frame_count = 240U,
        .output_channels = 2U,
        .amplifier_active_low = false,
        .probe_before_attach = true,
    };
}

}  // namespace micropixel::platform::reterminal_d1001::board