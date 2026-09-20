#include "apps/tilt/tilt_game.hpp"

namespace tilt {

void TiltGame::QueueProfile(std::span<const micropixel::ToneSpec> profile) {
    if (!tones_.Play(profile) && !audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("tilt: audio command dropped; visual gameplay continues");
    }
}

void TiltGame::AdvanceAudio(micropixel::Duration delta) { tones_.Advance(delta); }

void TiltGame::ClearAudioQueue() { (void)tones_.StopAll(); }

void TiltGame::PlayStartSound() { QueueProfile(tilt_sfx::kStart); }
void TiltGame::PlayWallSound() { QueueProfile(tilt_sfx::kWall); }
void TiltGame::PlayBumperSound() { QueueProfile(tilt_sfx::kBumper); }
void TiltGame::PlayStarSound() { QueueProfile(tilt_sfx::kStar); }

void TiltGame::PlayFallSound() {
    ClearAudioQueue();
    QueueProfile(tilt_sfx::kFall);
}

void TiltGame::PlayCompleteSound() {
    ClearAudioQueue();
    QueueProfile(tilt_sfx::kComplete);
}

}  // namespace tilt
