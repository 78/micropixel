#ifndef MICROPIXEL_APPS_MAZE_BREAK_MAZE_BREAK_AUDIO_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_MAZE_BREAK_AUDIO_HPP

#include <stdint.h>

#include "apps/maze-evil/audio/sound_ids.hpp"
#include "sdk/application.hpp"
#include "sdk/audio.hpp"
#include "sdk/event.hpp"

namespace maze_break {

// Game audio on top of the Host mixer: sound effects are tone profiles from
// audio/sfx.json played through the Host synth, the background loop is an Ogg
// Opus asset decoded by the Host. The Guest only schedules; no PCM is mixed
// here. Distance attenuation from the world scales `volume_per_mille`; the
// device master volume stays with the Host.
class GameAudio final {
   public:
    // `mute` disables every sound (SFX and BGM); `bgm_enabled` only the music.
    void Initialize(micropixel::Application& app, bool bgm_enabled, bool mute);

    void Play(audio::SoundEvent event);
    // Fires tones whose delay has elapsed; call once per frame.
    void Advance(uint64_t delta_us);
    void StartBgm();
    // The looping BGM only finishes when the Host stopped it (App pause); the
    // handle is dropped so the next StartBgm() restarts it.
    void OnPlaybackEvent(const micropixel::Event& event);
    void StopAll();

   private:
    struct ScheduledTone {
        micropixel::Tone tone{};
        uint64_t delay_us{};
        bool active{};
    };

    void Emit(const micropixel::Tone& tone);
    void NoteError();

    micropixel::Application* app_{};
    bool available_{};
    bool error_logged_{};
    micropixel::AudioClip bgm_clip_{};
    micropixel::Playback bgm_{};
    ScheduledTone scheduled_[24]{};
};

}  // namespace maze_break

#endif
