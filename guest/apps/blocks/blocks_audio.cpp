#include "apps/blocks/blocks_game.hpp"

namespace blocks {

void BlocksGame::QueueProfile(std::span<const micropixel::ToneSpec> profile) {
    if (!tones_.Play(profile) && !audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("blocks: audio command dropped; visual gameplay continues");
    }
}

void BlocksGame::AdvanceAudio(micropixel::Duration delta) { tones_.Advance(delta); }

void BlocksGame::ClearAudioQueue() {
    if (!tones_.StopAll() && !audio_error_logged_) {
        audio_error_logged_ = true;
        app_.log().Info("blocks: audio stop failed; gameplay continues");
    }
}

void BlocksGame::PlayStartSound() { QueueProfile(blocks_sfx::kStart); }

void BlocksGame::PlayMoveSound() { QueueProfile(blocks_sfx::kMove); }

void BlocksGame::PlayRotateSound() { QueueProfile(blocks_sfx::kRotate); }

void BlocksGame::PlayHoldSound() { QueueProfile(blocks_sfx::kHold); }

void BlocksGame::PlayLockSound(uint8_t drop_distance) {
    if (drop_distance != 0U) {
        QueueProfile(blocks_sfx::kHardDrop);
        return;
    }
    QueueProfile(blocks_sfx::kLock);
}

void BlocksGame::PlayLineSound(uint32_t lines, bool level_up) {
    if (level_up) {
        QueueProfile(blocks_sfx::kLevelUp);
        return;
    }
    // One note per cleared line, capped at the authored phrase length.
    const std::span<const micropixel::ToneSpec> line{blocks_sfx::kLine};
    QueueProfile(line.first(lines < line.size() ? lines : line.size()));
}

void BlocksGame::PlayGameOverSound() {
    ClearAudioQueue();
    QueueProfile(blocks_sfx::kGameOver);
}

}  // namespace blocks
