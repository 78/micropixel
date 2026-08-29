#include "apps/blocks/blocks_game.hpp"

namespace blocks {

BlocksGame::BlocksGame(micropixel::Application& app, micropixel::Renderer renderer,
                       micropixel::RendererInfo renderer_info, micropixel::Audio audio, bool audio_available,
                       uint32_t best_score)
    : app_(app),
      strings_(blocks_strings::ForLocale(app.localization().CurrentLocale())),
      renderer_(renderer),
      renderer_info_(renderer_info),
      audio_(audio),
      best_score_(best_score),
      audio_available_(audio_available) {
    screen_button_.SetBounds(kStartButtonRect);
    model_.Reset(kDefaultRandomSeed);
}

void BlocksGame::set_textures(micropixel::Texture board, micropixel::Texture start, micropixel::Texture restart) {
    micropixel::Assert(board.width() == static_cast<uint32_t>(kBoardAssetWidth) &&
                           board.height() == static_cast<uint32_t>(kBoardAssetHeight),
                       "blocks: board texture dimensions invalid");
    micropixel::Assert(start.width() == static_cast<uint32_t>(kActionButtonWidth) &&
                           start.height() == static_cast<uint32_t>(kActionButtonHeight) &&
                           restart.width() == static_cast<uint32_t>(kActionButtonWidth) &&
                           restart.height() == static_cast<uint32_t>(kActionButtonHeight),
                       "blocks: button texture dimensions invalid");
    board_texture_ = static_cast<micropixel::Texture&&>(board);
    start_button_texture_ = static_cast<micropixel::Texture&&>(start);
    restart_button_texture_ = static_cast<micropixel::Texture&&>(restart);
    InitializePlayfieldSurfaces();
}

void BlocksGame::StartNewGame() {
    model_.Reset(app_.random().U32());
    screen_ = Screen::kPlaying;
    gravity_accumulated_us_ = 0U;
    clear_effect_remaining_us_ = 0U;
    clear_rows_mask_ = 0U;
    clear_points_ = 0U;
    screen_button_.Reset();
    ResetGesture();
    ClearAudioQueue();
    PlayStartSound();
    Render();
}

void BlocksGame::EnterPause() {
    screen_ = Screen::kPaused;
    screen_button_.SetBounds(kStartButtonRect);
    screen_button_.Reset();
    ResetGesture();
    Render();
}

void BlocksGame::ResumeGame() {
    screen_ = Screen::kPlaying;
    gravity_accumulated_us_ = 0U;
    screen_button_.Reset();
    ResetGesture();
    PlayStartSound();
    Render();
}

void BlocksGame::EnterGameOver() {
    screen_ = Screen::kGameOver;
    screen_button_.SetBounds(kRestartButtonRect);
    screen_button_.Reset();
    ResetGesture();
    if (model_.score() > best_score_) {
        best_score_ = model_.score();
        if (!app_.storage().SetU32("best", best_score_).has_value() && !storage_error_logged_) {
            storage_error_logged_ = true;
            app_.log().Info("blocks: BEST persistence failed; gameplay continues");
        }
    }
    PlayGameOverSound();
}

void BlocksGame::HandleOutcome(const LockOutcome& outcome) {
    if (!outcome.locked) {
        return;
    }
    gravity_accumulated_us_ = 0U;
    if (outcome.cleared_lines != 0U) {
        clear_rows_mask_ = outcome.cleared_rows_mask;
        clear_points_ = outcome.points_gained;
        clear_effect_remaining_us_ = 240000U;
        PlayLineSound(outcome.cleared_lines, outcome.level_up);
    } else {
        PlayLockSound(outcome.drop_distance);
    }
    if (outcome.game_over) {
        EnterGameOver();
    }
}

void BlocksGame::OnTimer(const micropixel::TimerEvent& tick) {
    const uint64_t delta_us = tick.delta().count_microseconds();
    AdvanceAudio(delta_us);
    if (screen_ != Screen::kPlaying) {
        return;
    }
    if (clear_effect_remaining_us_ != 0U) {
        clear_effect_remaining_us_ =
            delta_us >= clear_effect_remaining_us_ ? 0U : clear_effect_remaining_us_ - delta_us;
        if (clear_effect_remaining_us_ == 0U) {
            Render();
        } else {
            SyncPlayfield();
        }
        return;
    }

    gravity_accumulated_us_ += delta_us;
    const uint64_t period_us = model_.drop_period_us();
    if (gravity_accumulated_us_ >= period_us) {
        gravity_accumulated_us_ -= period_us;
        if (gravity_accumulated_us_ >= period_us) {
            gravity_accumulated_us_ = 0U;
        }
        const LockOutcome outcome = model_.Tick();
        HandleOutcome(outcome);
        if (outcome.locked) {
            Render();
        } else if (outcome.moved) {
            SyncPlayfield();
        }
        return;
    }
}

void BlocksGame::ResetGesture() {
    gesture_active_ = false;
    gesture_moved_ = false;
    gesture_vertical_drag_ = false;
    gesture_started_in_pause_ = false;
    gesture_started_in_hold_ = false;
    gesture_touch_id_ = 0U;
}

void BlocksGame::HandlePlayGesture(const micropixel::TouchEvent& touch) {
    if (touch.phase() == micropixel::TouchPhase::kDown) {
        if (!kPlayTouchRect.contains(touch.x(), touch.y()) || gesture_active_) {
            return;
        }
        gesture_active_ = true;
        gesture_moved_ = false;
        gesture_vertical_drag_ = false;
        gesture_touch_id_ = touch.id();
        gesture_start_x_ = touch.x();
        gesture_start_y_ = touch.y();
        gesture_anchor_x_ = touch.x();
        gesture_anchor_y_ = touch.y();
        gesture_started_us_ = touch.timestamp().microseconds();
        gesture_started_in_pause_ = kPauseTouchRect.contains(touch.x(), touch.y());
        gesture_started_in_hold_ = kHoldTouchRect.contains(touch.x(), touch.y());
        return;
    }
    if (!gesture_active_ || touch.id() != gesture_touch_id_) {
        return;
    }

    const int32_t total_dx = static_cast<int32_t>(touch.x()) - gesture_start_x_;
    const int32_t total_dy = static_cast<int32_t>(touch.y()) - gesture_start_y_;
    const uint64_t elapsed_us = touch.timestamp().microseconds() - gesture_started_us_;
    if (touch.phase() == micropixel::TouchPhase::kMove) {
        int32_t dx = static_cast<int32_t>(touch.x()) - gesture_anchor_x_;
        int32_t dy = static_cast<int32_t>(touch.y()) - gesture_anchor_y_;
        bool played_move_sound = false;
        bool visual_changed = false;
        bool interface_changed = false;
        while (AbsoluteValue(dx) >= kCellPitch && AbsoluteValue(dx) > AbsoluteValue(dy)) {
            const int32_t direction = dx < 0 ? -1 : 1;
            if (model_.MoveHorizontal(direction)) {
                gesture_moved_ = true;
                visual_changed = true;
                if (!played_move_sound) {
                    PlayMoveSound();
                    played_move_sound = true;
                }
            }
            gesture_anchor_x_ += direction * kCellPitch;
            dx = static_cast<int32_t>(touch.x()) - gesture_anchor_x_;
        }
        while (elapsed_us >= 180000U && dy >= kCellPitch && AbsoluteValue(dy) > AbsoluteValue(dx)) {
            const LockOutcome outcome = model_.SoftDrop();
            gesture_moved_ = true;
            gesture_vertical_drag_ = true;
            HandleOutcome(outcome);
            if (outcome.moved && !played_move_sound) {
                PlayMoveSound();
                played_move_sound = true;
            }
            visual_changed = visual_changed || outcome.moved || outcome.locked;
            interface_changed = interface_changed || outcome.locked;
            gesture_anchor_y_ += kCellPitch;
            dy = static_cast<int32_t>(touch.y()) - gesture_anchor_y_;
            if (outcome.locked || screen_ != Screen::kPlaying) {
                ResetGesture();
                break;
            }
        }
        if (interface_changed) {
            Render();
        } else if (visual_changed) {
            SyncPlayfield();
        }
        return;
    }
    if (touch.phase() == micropixel::TouchPhase::kCancel) {
        ResetGesture();
        return;
    }
    if (touch.phase() != micropixel::TouchPhase::kUp) {
        return;
    }

    const bool was_moved = gesture_moved_;
    const bool was_vertical_drag = gesture_vertical_drag_;
    const bool started_in_pause = gesture_started_in_pause_;
    const bool started_in_hold = gesture_started_in_hold_;
    ResetGesture();
    bool interface_changed = false;
    bool visual_changed = false;
    if (!was_vertical_drag && total_dy >= 80 && elapsed_us <= 300000U &&
        AbsoluteValue(total_dy) > AbsoluteValue(total_dx)) {
        const LockOutcome outcome = model_.HardDrop();
        HandleOutcome(outcome);
        interface_changed = outcome.locked;
        visual_changed = outcome.moved || outcome.locked;
    } else if (!was_moved && total_dy <= -70 && AbsoluteValue(total_dy) > AbsoluteValue(total_dx)) {
        if (model_.Hold()) {
            gravity_accumulated_us_ = 0U;
            PlayHoldSound();
            interface_changed = true;
            visual_changed = true;
        }
    } else if (!was_moved && AbsoluteValue(total_dx) < 25 && AbsoluteValue(total_dy) < 25) {
        if (started_in_pause) {
            EnterPause();
            return;
        }
        if (started_in_hold && model_.Hold()) {
            gravity_accumulated_us_ = 0U;
            PlayHoldSound();
            interface_changed = true;
            visual_changed = true;
        } else if (!started_in_hold && model_.RotateClockwise()) {
            PlayRotateSound();
            visual_changed = true;
        }
    }
    if (interface_changed) {
        Render();
    } else if (visual_changed) {
        SyncPlayfield();
    }
}

void BlocksGame::OnTouch(const micropixel::TouchEvent& touch) {
    if (screen_ != Screen::kPlaying) {
        const micropixel::ui::ButtonUpdate update = screen_button_.OnTouch(touch);
        if (update.clicked) {
            if (screen_ == Screen::kMenu || screen_ == Screen::kGameOver) {
                StartNewGame();
            } else {
                ResumeGame();
            }
        } else if (update.redraw()) {
            Render();
        }
        return;
    }

    HandlePlayGesture(touch);
}

}  // namespace blocks
