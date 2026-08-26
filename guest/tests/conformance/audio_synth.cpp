#include "sdk/micropixel.hpp"

using micropixel::literals::operator""_ms;
using micropixel::literals::operator""_s;

namespace {

void WaitFor(micropixel::Application& app, micropixel::Duration delay) {
    micropixel::Timer timer = app.timers().After(delay);
    for (;;) {
        micropixel::Event event = app.WaitEvent();
        if (event.TimerFrom(timer) != nullptr) {
            return;
        }
    }
}

micropixel::Tone Tone(micropixel::Waveform waveform, uint32_t frequency_hz) {
    return micropixel::Tone{
        .waveform = waveform,
        .frequency_hz = frequency_hz,
        .duration = 260_ms,
        .volume_per_mille = 90U,
        .attack = 8_ms,
        .release = 60_ms,
    };
}

}  // namespace

int main() {
    micropixel::Application app;
    micropixel::Audio audio = app.audio();
    auto info = audio.info();
    micropixel::Assert(info.has_value(), "audio_synth: Audio 1.0 capability missing");
    micropixel::Assert(info.value().max_voices == 8U, "audio_synth: expected bounded 8-voice mixer");
    micropixel::Tone invalid = Tone(micropixel::Waveform::kSine, 440U);
    invalid.volume_per_mille = 1001U;
    micropixel::Assert(!audio.Play(invalid).has_value(), "audio_synth: invalid volume accepted");
    invalid = Tone(micropixel::Waveform::kSine, 19U);
    micropixel::Assert(!audio.Play(invalid).has_value(), "audio_synth: invalid frequency accepted");
    invalid = Tone(micropixel::Waveform::kSine, 440U);
    invalid.attack = 300_ms;
    micropixel::Assert(!audio.Play(invalid).has_value(), "audio_synth: invalid envelope accepted");
    app.log().Info("audio_synth: Audio 1.0 capability ready");

    // The Metalio-Claw4 codec module needs a short asynchronous power/mode setup. The
    // Service is already callable while it initializes, so wait before the
    // audible waveform sequence to make the manual conformance check deterministic.
    WaitFor(app, 3_s);

    const micropixel::Tone sequence[] = {
        Tone(micropixel::Waveform::kSine, 440U),
        Tone(micropixel::Waveform::kSquare, 554U),
        Tone(micropixel::Waveform::kTriangle, 659U),
        Tone(micropixel::Waveform::kNoise, 0U),
    };
    for (const micropixel::Tone& item : sequence) {
        micropixel::Assert(audio.Play(item).has_value(), "audio_synth: waveform command rejected");
        WaitFor(app, 340_ms);
    }

    constexpr uint32_t kChord[] = {262U, 330U, 392U, 523U, 659U, 784U, 1047U, 1319U};
    for (uint32_t frequency_hz : kChord) {
        micropixel::Tone voice = Tone(micropixel::Waveform::kSine, frequency_hz);
        voice.duration = 650_ms;
        voice.volume_per_mille = 35U;
        micropixel::Assert(audio.Play(voice).has_value(), "audio_synth: polyphonic command rejected");
    }
    WaitFor(app, 750_ms);
    micropixel::Assert(audio.StopAll().has_value(), "audio_synth: StopAll rejected");
    app.log().Info("audio_synth: waveform and 8-voice tests passed");
    return 0;
}
