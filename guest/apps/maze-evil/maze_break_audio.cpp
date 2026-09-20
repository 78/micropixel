#include "apps/maze-evil/maze_break_audio.hpp"

#include "maze-break_assets.hpp"
#include "maze-break_sfx_profiles.hpp"

namespace maze_break {
namespace {

constexpr uint16_t kBgmVolumePerMille = 520U;

using Profile = std::span<const micropixel::ToneSpec>;

Profile ProfileFor(audio::SoundId id) {
    using namespace maze_break_sfx;  // NOLINT(google-build-using-namespace)
    switch (id) {
        case audio::SoundId::kShotgun:
            return kShotgun;
        case audio::SoundId::kEmptyClick:
            return kEmptyClick;
        case audio::SoundId::kImpAlert:
            return kImpAlert;
        case audio::SoundId::kImpFireball:
            return kImpFireball;
        case audio::SoundId::kImpMelee:
            return kImpMelee;
        case audio::SoundId::kImpPain:
            return kImpPain;
        case audio::SoundId::kImpDeath:
            return kImpDeath;
        case audio::SoundId::kFireballExplode:
            return kFireballExplode;
        case audio::SoundId::kPlayerPain:
            return kPlayerPain;
        case audio::SoundId::kPickupHealth:
            return kPickupHealth;
        case audio::SoundId::kPickupAmmo:
            return kPickupAmmo;
        case audio::SoundId::kDoorOpen:
            return kDoorOpen;
        case audio::SoundId::kDoorClose:
            return kDoorClose;
        case audio::SoundId::kExitSealed:
            return kExitSealed;
        case audio::SoundId::kWin:
            return kWin;
        case audio::SoundId::kDie:
            return kDie;
        case audio::SoundId::kCount:
            break;
    }
    return {};
}

}  // namespace

void GameAudio::Initialize(bool bgm_enabled, bool mute) {
    micropixel::Application& app = *app_;
    tones_.set_enabled(false);
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
    tones_.set_enabled(true);
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

void GameAudio::Play(audio::SoundEvent event) {
    if (!available_ || event.gain < 8U) {
        return;
    }
    if (!tones_.Play(ProfileFor(event.id), event.gain)) {
        NoteError();
    }
}

void GameAudio::Advance(uint64_t delta_us) { tones_.Advance(micropixel::Duration::Microseconds(delta_us)); }

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
    bgm_.Reset();
    if (!tones_.StopAll()) {
        NoteError();
    }
}

}  // namespace maze_break
