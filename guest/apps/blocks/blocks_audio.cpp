#include "apps/blocks/blocks_game.hpp"

namespace blocks {

micropixel::Tone BlocksGame::SynthTone(micropixel::Waveform waveform, uint32_t frequency_hz, uint32_t duration_ms,
                                       uint16_t volume_per_mille, uint16_t attack_ms, uint16_t release_ms) const {
    const uint16_t volume =
        static_cast<uint16_t>((static_cast<uint32_t>(volume_per_mille) * blocks_sfx::kMasterPercent + 50U) / 100U);
    return micropixel::Tone{waveform,
                            frequency_hz,
                            micropixel::Duration::Milliseconds(duration_ms),
                            volume,
                            micropixel::Duration::Milliseconds(attack_ms),
                            micropixel::Duration::Milliseconds(release_ms)};
}

void BlocksGame::EmitTone(const micropixel::Tone& tone) {
    if (audio_available_ && !audio_.Play(tone).has_value() && !audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("blocks: audio command dropped; visual gameplay continues");
    }
}

void BlocksGame::QueueTone(const micropixel::Tone& tone, uint32_t delay_ms) {
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

void BlocksGame::QueueProfile(const blocks_sfx::ToneSpec* tones, uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        const blocks_sfx::ToneSpec& tone = tones[index];
        QueueTone(SynthTone(tone.waveform, tone.frequency_hz, tone.duration_ms, tone.volume_per_mille, tone.attack_ms,
                            tone.release_ms),
                  tone.delay_ms);
    }
}

void BlocksGame::AdvanceAudio(uint64_t delta_us) {
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

void BlocksGame::ClearAudioQueue() {
    for (ScheduledTone& scheduled : scheduled_tones_) {
        scheduled.active = false;
    }
    if (audio_available_ && !audio_.StopAll().has_value() && !audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("blocks: audio stop failed; gameplay continues");
    }
}

void BlocksGame::PlayStartSound() {
    QueueProfile(blocks_sfx::kStart, blocks_sfx::kStartCount);
}

void BlocksGame::PlayMoveSound() {
    QueueProfile(blocks_sfx::kMove, blocks_sfx::kMoveCount);
}

void BlocksGame::PlayRotateSound() {
    QueueProfile(blocks_sfx::kRotate, blocks_sfx::kRotateCount);
}

void BlocksGame::PlayHoldSound() {
    QueueProfile(blocks_sfx::kHold, blocks_sfx::kHoldCount);
}

void BlocksGame::PlayLockSound(uint8_t drop_distance) {
    if (drop_distance != 0U) {
        QueueProfile(blocks_sfx::kHardDrop, blocks_sfx::kHardDropCount);
        return;
    }
    QueueProfile(blocks_sfx::kLock, blocks_sfx::kLockCount);
}

void BlocksGame::PlayLineSound(uint32_t lines, bool level_up) {
    if (level_up) {
        QueueProfile(blocks_sfx::kLevelUp, blocks_sfx::kLevelUpCount);
        return;
    }
    const uint32_t notes = lines < blocks_sfx::kLineCount ? lines : blocks_sfx::kLineCount;
    QueueProfile(blocks_sfx::kLine, notes);
}

void BlocksGame::PlayGameOverSound() {
    ClearAudioQueue();
    QueueProfile(blocks_sfx::kGameOver, blocks_sfx::kGameOverCount);
}

}  // namespace blocks
