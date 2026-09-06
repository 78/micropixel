#include "apps/maze-evil/maze_break_audio.hpp"

#include "maze-break_assets.hpp"
#include "maze-break_sfx_profiles.hpp"

namespace maze_break {
namespace {

constexpr uint16_t kBgmVolumePerMille = 520U;

struct Profile {
    const maze_break_sfx::ToneSpec* tones;
    uint32_t count;
};

Profile ProfileFor(audio::SoundId id) {
    using namespace maze_break_sfx;  // NOLINT(google-build-using-namespace)
    switch (id) {
        case audio::SoundId::kShotgun:
            return {kShotgun, kShotgunCount};
        case audio::SoundId::kEmptyClick:
            return {kEmptyClick, kEmptyClickCount};
        case audio::SoundId::kImpAlert:
            return {kImpAlert, kImpAlertCount};
        case audio::SoundId::kImpFireball:
            return {kImpFireball, kImpFireballCount};
        case audio::SoundId::kImpMelee:
            return {kImpMelee, kImpMeleeCount};
        case audio::SoundId::kImpPain:
            return {kImpPain, kImpPainCount};
        case audio::SoundId::kImpDeath:
            return {kImpDeath, kImpDeathCount};
        case audio::SoundId::kFireballExplode:
            return {kFireballExplode, kFireballExplodeCount};
        case audio::SoundId::kPlayerPain:
            return {kPlayerPain, kPlayerPainCount};
        case audio::SoundId::kPickupHealth:
            return {kPickupHealth, kPickupHealthCount};
        case audio::SoundId::kPickupAmmo:
            return {kPickupAmmo, kPickupAmmoCount};
        case audio::SoundId::kDoorOpen:
            return {kDoorOpen, kDoorOpenCount};
        case audio::SoundId::kDoorClose:
            return {kDoorClose, kDoorCloseCount};
        case audio::SoundId::kExitSealed:
            return {kExitSealed, kExitSealedCount};
        case audio::SoundId::kWin:
            return {kWin, kWinCount};
        case audio::SoundId::kDie:
            return {kDie, kDieCount};
        case audio::SoundId::kCount:
            break;
    }
    return {nullptr, 0U};
}

}  // namespace

void GameAudio::Initialize(micropixel::Application& app, bool bgm_enabled, bool mute) {
    app_ = &app;
    if (mute) {
        // Benchmarks and --mute: never touch the Audio service.
        available_ = false;
        app.log().Info("maze-break: audio muted");
        return;
    }
    auto info = app.audio().info();
    available_ = info.has_value();
    if (!available_) {
        app.log().Info("maze-break: audio unavailable; playing silent");
        return;
    }
    if (bgm_enabled && info->supports_ogg_opus) {
        auto clip = app.audio().Load(maze_break_assets::bgm_loop);
        if (clip.has_value()) {
            bgm_clip_ = static_cast<micropixel::AudioClip&&>(clip.value());
        } else {
            app.log().Info("maze-break: BGM clip failed to load; continuing without music");
        }
    }
}

void GameAudio::NoteError() {
    if (!error_logged_ && app_ != nullptr) {
        error_logged_ = true;
        app_->log().Info("maze-break: audio command dropped; gameplay continues");
    }
}

void GameAudio::Emit(const micropixel::Tone& tone) {
    if (available_ && !app_->audio().Play(tone).has_value()) {
        NoteError();
    }
}

void GameAudio::Play(audio::SoundEvent event) {
    if (!available_ || event.gain < 8U) {
        return;
    }
    const Profile profile = ProfileFor(event.id);
    for (uint32_t index = 0U; index < profile.count; ++index) {
        const maze_break_sfx::ToneSpec& spec = profile.tones[index];
        const micropixel::Tone tone{
            spec.waveform,
            spec.frequency_hz,
            micropixel::Duration::Milliseconds(spec.duration_ms),
            static_cast<uint16_t>((static_cast<uint32_t>(spec.volume_per_mille) * event.gain + 127U) / 255U),
            micropixel::Duration::Milliseconds(spec.attack_ms),
            micropixel::Duration::Milliseconds(spec.release_ms),
        };
        if (spec.delay_ms == 0U) {
            Emit(tone);
            continue;
        }
        bool queued = false;
        for (ScheduledTone& slot : scheduled_) {
            if (!slot.active) {
                slot = ScheduledTone{tone, static_cast<uint64_t>(spec.delay_ms) * 1000U, true};
                queued = true;
                break;
            }
        }
        if (!queued) {
            NoteError();
        }
    }
}

void GameAudio::Advance(uint64_t delta_us) {
    if (!available_) {
        return;
    }
    for (ScheduledTone& slot : scheduled_) {
        if (!slot.active) {
            continue;
        }
        if (delta_us >= slot.delay_us) {
            slot.active = false;
            Emit(slot.tone);
        } else {
            slot.delay_us -= delta_us;
        }
    }
}

void GameAudio::StartBgm() {
    if (!available_ || !bgm_clip_.valid() || bgm_.valid()) {
        return;
    }
    auto playback = app_->audio().Play(bgm_clip_, micropixel::PlaybackOptions{kBgmVolumePerMille, true});
    if (playback.has_value()) {
        bgm_ = static_cast<micropixel::Playback&&>(playback.value());
    } else {
        NoteError();
    }
}

void GameAudio::OnPlaybackEvent(const micropixel::Event& event) {
    if (bgm_.valid() && event.PlaybackFrom(bgm_) != nullptr) {
        bgm_.Reset();
    }
}

void GameAudio::StopAll() {
    for (ScheduledTone& slot : scheduled_) {
        slot.active = false;
    }
    bgm_.Reset();
    if (available_ && !app_->audio().StopAll().has_value()) {
        NoteError();
    }
}

}  // namespace maze_break
