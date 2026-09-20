#include "apps/snake/snake_game.hpp"

namespace snake {

void SnakeGame::NoteAudioError() {
    if (!audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("snake: audio command dropped; visual gameplay continues");
    }
}

void SnakeGame::QueueProfile(std::span<const micropixel::ToneSpec> profile) {
    if (!tones_.Play(profile)) {
        NoteAudioError();
    }
}

void SnakeGame::StopAudio() {
    bgm_playing_ = false;
    if (!tones_.StopAll()) {
        NoteAudioError();
    }
}

void SnakeGame::StartBgm() {
    if (!tones_.enabled() || !bgm_enabled_) {
        return;
    }
    bgm_playing_ = true;
    bgm_note_index_ = 0U;
    bgm_remaining_us_ = 50000U;
}

void SnakeGame::AdvanceAudio(uint64_t delta_us) {
    if (!tones_.enabled()) {
        return;
    }
    tones_.Advance(micropixel::Duration::Microseconds(delta_us));
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
    if (!tones_.PlayNow(note.ToTone())) {
        NoteAudioError();
    }
    bgm_note_index_ = (bgm_note_index_ + 1U) % total_notes;
    uint32_t beats_per_minute = 140U + (model_.level() - 1U) * 10U;
    uint64_t interval_us = 30000000ULL / beats_per_minute;
    uint64_t overshoot_us = delta_us - bgm_remaining_us_;
    bgm_remaining_us_ = interval_us > overshoot_us ? interval_us - overshoot_us : 1000U;
}

void SnakeGame::PlayStartSound() { QueueProfile(snake_sfx::kStart); }

void SnakeGame::PlayFoodSound(FoodType type) {
    switch (type) {
        case FoodType::kNormal:
            QueueProfile(snake_sfx::kFoodNormal);
            break;
        case FoodType::kGolden:
            QueueProfile(snake_sfx::kFoodGolden);
            break;
        case FoodType::kPoison:
            QueueProfile(snake_sfx::kFoodPoison);
            break;
        case FoodType::kSpeed:
            QueueProfile(snake_sfx::kFoodSpeed);
            break;
    }
}

void SnakeGame::PlayLevelUpSound() { QueueProfile(snake_sfx::kLevelUp); }

void SnakeGame::PlayDieSound() { QueueProfile(snake_sfx::kGameOver); }

}  // namespace snake
