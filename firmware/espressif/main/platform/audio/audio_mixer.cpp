#include "platform/audio/audio_mixer.hpp"

#include <cmath>

namespace micropixel::platform::audio {
namespace {

int32_t WaveformSample(SynthVoice& voice, const int16_t (&sine_table)[kSynthSineTableSize]) {
    switch (voice.waveform) {
        case MICROPIXEL_AUDIO_WAVE_SINE:
            return sine_table[voice.phase >> 24U];
        case MICROPIXEL_AUDIO_WAVE_SQUARE:
            return (voice.phase & 0x80000000U) == 0U ? 32767 : -32768;
        case MICROPIXEL_AUDIO_WAVE_TRIANGLE: {
            const uint32_t position = voice.phase >> 16U;
            return position < 32768U ? static_cast<int32_t>(position * 2U) - 32768
                                     : 98303 - static_cast<int32_t>(position * 2U);
        }
        case MICROPIXEL_AUDIO_WAVE_NOISE:
            voice.noise = voice.noise * 1664525U + 1013904223U;
            return static_cast<int16_t>(voice.noise >> 16U);
        default:
            return 0;
    }
}

}  // namespace

void AudioMixer::InitializeSineTable(int16_t (&table)[kSynthSineTableSize]) {
    constexpr double kTau = 6.28318530717958647692;
    for (uint32_t index = 0U; index < kSynthSineTableSize; ++index) {
        table[index] =
            static_cast<int16_t>(std::sin(kTau * static_cast<double>(index) / kSynthSineTableSize) * 32767.0);
    }
}

bool AudioMixer::ValidTone(const micropixel_audio_tone_t& tone) {
    return tone.size == sizeof(tone) && tone.interface_major == MICROPIXEL_AUDIO_INTERFACE_MAJOR &&
           tone.waveform >= MICROPIXEL_AUDIO_WAVE_SINE && tone.waveform <= MICROPIXEL_AUDIO_WAVE_NOISE &&
           tone.volume_per_mille <= 1000U && tone.duration_ms != 0U &&
           tone.duration_ms <= MICROPIXEL_AUDIO_MAX_TONE_DURATION_MS && tone.attack_ms <= tone.duration_ms &&
           tone.release_ms <= tone.duration_ms && tone.reserved[0] == 0U && tone.reserved[1] == 0U &&
           tone.reserved[2] == 0U &&
           (tone.waveform == MICROPIXEL_AUDIO_WAVE_NOISE ||
            (tone.frequency_millihz >= 20000U && tone.frequency_millihz <= 20000000U));
}

void AudioMixer::StartVoice(SynthVoice& voice, const micropixel_audio_tone_t& tone, uint32_t sample_rate) {
    const uint64_t frequency = tone.waveform == MICROPIXEL_AUDIO_WAVE_NOISE ? 0U : tone.frequency_millihz;
    voice = SynthVoice{
        .waveform = tone.waveform,
        .phase = 0U,
        .phase_step = static_cast<uint32_t>(frequency * (1ULL << 32U) / (static_cast<uint64_t>(sample_rate) * 1000ULL)),
        .total_frames = tone.duration_ms * sample_rate / 1000U,
        .remaining_frames = tone.duration_ms * sample_rate / 1000U,
        .attack_frames = tone.attack_ms * sample_rate / 1000U,
        .release_frames = tone.release_ms * sample_rate / 1000U,
        .volume_per_mille = tone.volume_per_mille,
        .noise = 0x51a9e21dU ^ tone.frequency_millihz ^ tone.duration_ms,
        .active = true,
    };
}

int32_t AudioMixer::NextSample(SynthVoice& voice, const int16_t (&sine_table)[kSynthSineTableSize]) {
    const uint32_t played_frames = voice.total_frames - voice.remaining_frames;
    uint32_t gain = voice.volume_per_mille;
    if (voice.attack_frames != 0U && played_frames < voice.attack_frames) {
        gain = gain * played_frames / voice.attack_frames;
    }
    if (voice.release_frames != 0U && voice.remaining_frames < voice.release_frames) {
        const uint32_t release_gain = voice.volume_per_mille * voice.remaining_frames / voice.release_frames;
        if (release_gain < gain) {
            gain = release_gain;
        }
    }
    const int32_t sample = WaveformSample(voice, sine_table) * static_cast<int32_t>(gain) / 1000;
    voice.phase += voice.phase_step;
    --voice.remaining_frames;
    if (voice.remaining_frames == 0U) {
        voice.active = false;
    }
    return sample;
}

}  // namespace micropixel::platform::audio
