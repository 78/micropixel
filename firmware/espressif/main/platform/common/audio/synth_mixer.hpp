#pragma once

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::platform::common::audio {

constexpr uint32_t kSynthSineTableSize = 256U;

struct SynthVoice final {
    uint32_t waveform{};
    uint32_t phase{};
    uint32_t phase_step{};
    uint32_t total_frames{};
    uint32_t remaining_frames{};
    uint32_t attack_frames{};
    uint32_t release_frames{};
    uint32_t volume_per_mille{};
    uint32_t noise{0x51a9e21dU};
    bool active{};
};

class SynthMixer final {
   public:
    static void InitializeSineTable(int16_t (&table)[kSynthSineTableSize]);
    [[nodiscard]] static bool ValidTone(const micropixel_audio_tone_t& tone);
    static void StartVoice(SynthVoice& voice, const micropixel_audio_tone_t& tone, uint32_t sample_rate);
    [[nodiscard]] static int32_t NextSample(SynthVoice& voice, const int16_t (&sine_table)[kSynthSineTableSize]);
};

}  // namespace micropixel::platform::common::audio
