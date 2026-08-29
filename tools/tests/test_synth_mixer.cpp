#include <cstdlib>
#include <iostream>
#include <limits>

#include "platform/audio/audio_mixer.hpp"
#include "platform/audio/audio_output_policy.hpp"

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

void AudioOutputLifecycleIsBounded() {
    using micropixel::platform::audio::NextAudioIdleChunkCount;
    using micropixel::platform::audio::ShouldRunAudioOutput;
    using micropixel::platform::audio::ShouldStopAudioOutput;

    Check(ShouldRunAudioOutput(true, false), "a foreground App must keep audio output running");
    Check(!ShouldRunAudioOutput(false, false), "idle background audio output must stay stopped");
    Check(ShouldRunAudioOutput(false, true), "a playable voice must keep background output running");

    constexpr uint32_t kGraceChunks = 3U;
    uint32_t idle_chunks = 0U;
    for (uint32_t index = 0U; index < 20U; ++index) {
        idle_chunks = NextAudioIdleChunkCount(idle_chunks, true, false, false);
        Check(idle_chunks == 0U, "foreground output must reset the idle counter");
        Check(!ShouldStopAudioOutput(true, idle_chunks, kGraceChunks, false),
              "foreground output must not stop at the idle threshold");
    }

    idle_chunks = NextAudioIdleChunkCount(idle_chunks, false, false, false);
    Check(idle_chunks == 1U && !ShouldStopAudioOutput(false, idle_chunks, kGraceChunks, false),
          "the first idle chunk must remain inside the grace period");
    idle_chunks = NextAudioIdleChunkCount(idle_chunks, false, false, false);
    Check(idle_chunks == 2U && !ShouldStopAudioOutput(false, idle_chunks, kGraceChunks, false),
          "the second idle chunk must remain inside the grace period");
    idle_chunks = NextAudioIdleChunkCount(idle_chunks, false, false, false);
    Check(idle_chunks == kGraceChunks && ShouldStopAudioOutput(false, idle_chunks, kGraceChunks, false),
          "audio output must stop after the complete idle grace period");

    Check(NextAudioIdleChunkCount(kGraceChunks, false, true, false) == 0U,
          "rendered audio must reset the idle counter");
    Check(NextAudioIdleChunkCount(kGraceChunks, false, false, true) == 0U,
          "a playable voice must reset the idle counter");
    Check(NextAudioIdleChunkCount(kGraceChunks, true, false, false) == 0U,
          "a foreground App must reset the idle counter");
    Check(NextAudioIdleChunkCount(std::numeric_limits<uint32_t>::max(), false, false, false) ==
              std::numeric_limits<uint32_t>::max(),
          "the idle counter must saturate instead of wrapping");
}

}  // namespace

int main() {
    using micropixel::platform::audio::kSynthSineTableSize;
    using micropixel::platform::audio::AudioMixer;
    using micropixel::platform::audio::SynthVoice;

    int16_t sine_table[kSynthSineTableSize]{};
    AudioMixer::InitializeSineTable(sine_table);
    Check(sine_table[0] == 0, "sine table must begin at zero");
    Check(sine_table[kSynthSineTableSize / 4U] > 32000, "sine table quarter period must be near full scale");

    micropixel_audio_tone_t tone = Tone();
    Check(AudioMixer::ValidTone(tone), "valid tone was rejected");
    tone.reserved[1] = 1U;
    Check(!AudioMixer::ValidTone(tone), "reserved fields must be zero");
    tone = Tone();
    tone.volume_per_mille = 1001U;
    Check(!AudioMixer::ValidTone(tone), "volume above per-mille range must be rejected");

    tone = Tone(MICROPIXEL_AUDIO_WAVE_SQUARE);
    SynthVoice voice{};
    AudioMixer::StartVoice(voice, tone, 1000U);
    Check(voice.active && voice.total_frames == 10U, "voice duration must use the requested sample rate");
    Check(AudioMixer::NextSample(voice, sine_table) == 0, "attack must begin at zero gain");
    for (uint32_t frame = 1U; frame < 10U; ++frame) {
        (void)AudioMixer::NextSample(voice, sine_table);
    }
    Check(!voice.active && voice.remaining_frames == 0U, "voice must stop at the final frame");

    AudioOutputLifecycleIsBounded();

    std::cout << "audio mixer and output policy tests passed\n";
    return 0;
}
