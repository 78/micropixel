#include "apps/tilt/tilt_game.hpp"

namespace tilt {

micropixel::Tone TiltGame::SynthTone(micropixel::Waveform waveform, uint32_t frequency_hz, uint32_t duration_ms,
                                     uint16_t volume_per_mille, uint16_t attack_ms, uint16_t release_ms) const {
    return {waveform,
            frequency_hz,
            micropixel::Duration::Milliseconds(duration_ms),
            volume_per_mille,
            micropixel::Duration::Milliseconds(attack_ms),
            micropixel::Duration::Milliseconds(release_ms)};
}

void TiltGame::EmitTone(const micropixel::Tone& tone) {
    if (audio_available_ && !audio_.Play(tone).has_value() && !audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("tilt: audio command dropped; visual gameplay continues");
    }
}

void TiltGame::QueueTone(const micropixel::Tone& tone, uint32_t delay_ms) {
    if (!audio_available_) {
        return;
    }
    if (delay_ms == 0U) {
        EmitTone(tone);
        return;
    }
    for (ScheduledTone& scheduled : scheduled_tones_) {
        if (!scheduled.active) {
            scheduled.tone = tone;
            scheduled.delay_us = static_cast<uint64_t>(delay_ms) * 1000U;
            scheduled.active = true;
            return;
        }
    }
}

void TiltGame::QueueProfile(const tilt_sfx::ToneSpec* tones, uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        const tilt_sfx::ToneSpec& tone = tones[index];
        QueueTone(SynthTone(tone.waveform, tone.frequency_hz, tone.duration_ms, tone.volume_per_mille, tone.attack_ms,
                            tone.release_ms),
                  tone.delay_ms);
    }
}

void TiltGame::AdvanceAudio(uint64_t delta_us) {
    for (ScheduledTone& scheduled : scheduled_tones_) {
        if (!scheduled.active) {
            continue;
        }
        if (delta_us >= scheduled.delay_us) {
            scheduled.active = false;
            EmitTone(scheduled.tone);
        } else {
            scheduled.delay_us -= delta_us;
        }
    }
}

void TiltGame::ClearAudioQueue() {
    for (ScheduledTone& scheduled : scheduled_tones_) {
        scheduled.active = false;
    }
    if (audio_available_) {
        (void)audio_.StopAll();
    }
}

void TiltGame::PlayStartSound() { QueueProfile(tilt_sfx::kStart, tilt_sfx::kStartCount); }
void TiltGame::PlayWallSound() { QueueProfile(tilt_sfx::kWall, tilt_sfx::kWallCount); }
void TiltGame::PlayBumperSound() { QueueProfile(tilt_sfx::kBumper, tilt_sfx::kBumperCount); }
void TiltGame::PlayStarSound() { QueueProfile(tilt_sfx::kStar, tilt_sfx::kStarCount); }

void TiltGame::PlayFallSound() {
    ClearAudioQueue();
    QueueProfile(tilt_sfx::kFall, tilt_sfx::kFallCount);
}

void TiltGame::PlayCompleteSound() {
    ClearAudioQueue();
    QueueProfile(tilt_sfx::kComplete, tilt_sfx::kCompleteCount);
}

}  // namespace tilt
