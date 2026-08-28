#include <cstdlib>
#include <iostream>

#include "platform/common/audio/synth_mixer.hpp"

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

micropixel_audio_tone_t Tone(uint16_t waveform = MICROPIXEL_AUDIO_WAVE_SINE) {
    return {
        .size = sizeof(micropixel_audio_tone_t),
        .interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR,
        .waveform = waveform,
        .volume_per_mille = 800U,
        .frequency_millihz = 440000U,
        .duration_ms = 10U,
        .attack_ms = 2U,
        .release_ms = 2U,
        .reserved = {},
    };
}

}  // namespace

int main() {
    using micropixel::platform::common::audio::SynthMixer;
    using micropixel::platform::common::audio::SynthVoice;
    using micropixel::platform::common::audio::kSynthSineTableSize;

    int16_t sine_table[kSynthSineTableSize]{};
    SynthMixer::InitializeSineTable(sine_table);
    Check(sine_table[0] == 0, "sine table must begin at zero");
    Check(sine_table[kSynthSineTableSize / 4U] > 32000, "sine table quarter period must be near full scale");

    micropixel_audio_tone_t tone = Tone();
    Check(SynthMixer::ValidTone(tone), "valid tone was rejected");
    tone.reserved[1] = 1U;
    Check(!SynthMixer::ValidTone(tone), "reserved fields must be zero");
    tone = Tone();
    tone.volume_per_mille = 1001U;
    Check(!SynthMixer::ValidTone(tone), "volume above per-mille range must be rejected");

    tone = Tone(MICROPIXEL_AUDIO_WAVE_SQUARE);
    SynthVoice voice{};
    SynthMixer::StartVoice(voice, tone, 1000U);
    Check(voice.active && voice.total_frames == 10U, "voice duration must use the requested sample rate");
    Check(SynthMixer::NextSample(voice, sine_table) == 0, "attack must begin at zero gain");
    for (uint32_t frame = 1U; frame < 10U; ++frame) {
        (void)SynthMixer::NextSample(voice, sine_table);
    }
    Check(!voice.active && voice.remaining_frames == 0U, "voice must stop at the final frame");

    std::cout << "synth mixer tests passed\n";
    return 0;
}
