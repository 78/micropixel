#include "apps/snake/snake_game.hpp"

namespace snake {

micropixel::Tone SnakeGame::SynthTone(micropixel::Waveform waveform, uint32_t frequency_hz, uint32_t duration_ms,
                                      uint16_t volume_per_mille, uint16_t attack_ms, uint16_t release_ms) {
    return micropixel::Tone{
        waveform,
        frequency_hz,
        micropixel::Duration::Milliseconds(duration_ms),
        volume_per_mille,
        micropixel::Duration::Milliseconds(attack_ms),
        micropixel::Duration::Milliseconds(release_ms),
    };
}

void SnakeGame::NoteAudioError() {
    if (!audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("snake: audio command dropped; visual gameplay continues");
    }
}

void SnakeGame::EmitTone(const micropixel::Tone& tone) {
    if (audio_available_ && !audio_.Play(tone).has_value()) {
        NoteAudioError();
    }
}

void SnakeGame::QueueTone(const micropixel::Tone& tone, uint32_t delay_ms) {
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
    NoteAudioError();
}

void SnakeGame::QueueProfile(const snake_sfx::ToneSpec* tones, uint32_t count) {
    for (uint32_t index = 0U; index < count; ++index) {
        const snake_sfx::ToneSpec& tone = tones[index];
        QueueTone(SynthTone(tone.waveform, tone.frequency_hz, tone.duration_ms, tone.volume_per_mille, tone.attack_ms,
                            tone.release_ms),
                  tone.delay_ms);
    }
}

void SnakeGame::ClearScheduledTones() {
    for (ScheduledTone& scheduled : scheduled_tones_) {
        scheduled.active = false;
    }
}

void SnakeGame::StopAudio() {
    bgm_playing_ = false;
    ClearScheduledTones();
    if (audio_available_ && !audio_.StopAll().has_value()) {
        NoteAudioError();
    }
}

void SnakeGame::StartBgm() {
    if (!audio_available_) {
        return;
    }
    bgm_playing_ = true;
    bgm_note_index_ = 0U;
    bgm_remaining_us_ = 50000U;
}

void SnakeGame::AdvanceAudio(uint64_t delta_us) {
    if (!audio_available_) {
        return;
    }
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
    if (!bgm_playing_) {
        return;
    }
    if (delta_us < bgm_remaining_us_) {
        bgm_remaining_us_ -= delta_us;
        return;
    }
    const uint32_t total_notes = snake_sfx::kBgmACount + snake_sfx::kBgmBCount;
    const snake_sfx::ToneSpec& note = bgm_note_index_ < snake_sfx::kBgmACount
                                          ? snake_sfx::kBgmA[bgm_note_index_]
                                          : snake_sfx::kBgmB[bgm_note_index_ - snake_sfx::kBgmACount];
    // JSON delays model a representative 140 BPM phrase for analysis. The
    // runtime owns cadence so the melody can still accelerate with the level.
    EmitTone(SynthTone(note.waveform, note.frequency_hz, note.duration_ms, note.volume_per_mille, note.attack_ms,
                       note.release_ms));
    bgm_note_index_ = (bgm_note_index_ + 1U) % total_notes;
    uint32_t beats_per_minute = 140U + (model_.level() - 1U) * 10U;
    uint64_t interval_us = 30000000ULL / beats_per_minute;
    uint64_t overshoot_us = delta_us - bgm_remaining_us_;
    bgm_remaining_us_ = interval_us > overshoot_us ? interval_us - overshoot_us : 1000U;
}

void SnakeGame::PlayStartSound() { QueueProfile(snake_sfx::kStart, snake_sfx::kStartCount); }

void SnakeGame::PlayFoodSound(FoodType type) {
    switch (type) {
        case FoodType::kNormal:
            QueueProfile(snake_sfx::kFoodNormal, snake_sfx::kFoodNormalCount);
            break;
        case FoodType::kGolden:
            QueueProfile(snake_sfx::kFoodGolden, snake_sfx::kFoodGoldenCount);
            break;
        case FoodType::kPoison:
            QueueProfile(snake_sfx::kFoodPoison, snake_sfx::kFoodPoisonCount);
            break;
        case FoodType::kSpeed:
            QueueProfile(snake_sfx::kFoodSpeed, snake_sfx::kFoodSpeedCount);
            break;
    }
}

void SnakeGame::PlayLevelUpSound() { QueueProfile(snake_sfx::kLevelUp, snake_sfx::kLevelUpCount); }

void SnakeGame::PlayDieSound() { QueueProfile(snake_sfx::kGameOver, snake_sfx::kGameOverCount); }

}  // namespace snake
