#include "apps/snake/snake_game.hpp"

namespace snake {

SnakeGame::SnakeGame(micropixel::Application& app, micropixel::Renderer renderer,
                     micropixel::RendererInfo renderer_info, micropixel::Audio audio, bool audio_available,
                     uint32_t best_score)
    : app_(app),
      renderer_(renderer),
      renderer_info_(renderer_info),
      audio_(audio),
      best_score_(best_score),
      audio_available_(audio_available) {
    screen_button_.SetBounds(kStartButtonRect);
    pause_touch_button_.SetBounds(kPauseTouchRect);
    ResetGameModel();
    SnapshotBody();
    ResetBodySlotMapping();
    app_.log().Info("snake: M18 menu ready");
    app_.log().Info("snake: renderer state translation enabled");
}

void SnakeGame::OnTimer(const micropixel::TimerEvent& tick) {
    uint64_t delta_us = tick.delta().count_microseconds();
    AdvanceAudio(delta_us);
    if (screen_ != Screen::kPlaying) {
        // A collision can enter GameOver while a Surface shake is active.
        // Keep driving only that visual effect so the Host can restore its
        // framebuffers and leave direct-compositor mode cleanly.
        if (shake_remaining_us_ != 0U) {
            animation_time_us_ += delta_us;
            AdvanceEffects(delta_us);
            Render();
        }
        return;
    }
    animation_time_us_ += delta_us;
    // Golden bursts cover several cells around the head. Treat them like the
    // full-board upgrade shake: keep accepting queued turns, but do not move
    // the snake underneath an effect that hides the route ahead.
    const bool movement_effect_was_active =
        BlocksMovementDuringEffects(shake_remaining_us_, burst_remaining_us_, burst_type_);
    AdvanceEffects(delta_us);
    const bool movement_effect_is_active =
        BlocksMovementDuringEffects(shake_remaining_us_, burst_remaining_us_, burst_type_);
    if (level_banner_us_ != 0U) {
        level_banner_us_ = delta_us >= level_banner_us_ ? 0U : level_banner_us_ - delta_us;
    }
    if (movement_effect_was_active) {
        // The Surface compositor intentionally moves a frozen board snapshot.
        // Golden bursts likewise obscure the cells immediately ahead. Hold the
        // next logical Move, but finish presenting the Move that triggered the
        // effect so the head reaches the food through ordinary interpolation.
        const uint64_t period_us = MovementPeriodUs();
        accumulated_us_ = AdvanceHeldMotion(accumulated_us_, delta_us, period_us);
        if (!movement_effect_is_active && accumulated_us_ >= period_us) {
            // The triggering Move is now fully visible. Rebase interpolation
            // at its destination and start a fresh period for the next Move;
            // otherwise the saturated phase would advance again immediately
            // on the first frame After the effect.
            ResetBodySlotMapping();
            accumulated_us_ = 0U;
        }
        Render();
        return;
    }
    (void)model_.AdvanceTime(delta_us);
    accumulated_us_ += delta_us;
    const uint64_t period_us = MovementPeriodUs();
    if (screen_ == Screen::kPlaying && accumulated_us_ >= period_us) {
        accumulated_us_ -= period_us;
        // Periodic Timer events are coalesced while a slow Render holds the
        // Guest. Never replay multiple stale movement ticks in one visible
        // frame: preserve ordinary sub-period jitter, but discard a whole
        // extra period of debt After a long frame. This prevents a burst
        // effect from ending with a sudden catch-up sprint.
        if (accumulated_us_ >= period_us) {
            accumulated_us_ = 0U;
            if (!logic_debt_logged_) {
                logic_debt_logged_ = true;
                app_.log().Info("snake: dropped coalesced movement debt After a slow frame");
            }
        }
        Cell previous_head = model_.body()[0];
        uint32_t previous_length = model_.length();
        SnapshotBody();
        MoveOutcome outcome = model_.Move();
        if (outcome.changed) {
            AdvanceBodySlotMapping(previous_head, previous_length);
        }
        if (outcome.changed) {
            SpawnTrail(previous_head);
        }
        if (outcome.ate) {
            uint32_t previous_best = best_score_;
            if (model_.score() > best_score_) {
                best_score_ = model_.score();
                best_dirty_ = true;
            }
            TriggerFoodEffects(model_.body()[0], outcome);
            PlayFoodSound(outcome.food_type);
            if (!record_broken_ && previous_best != 0U && model_.score() > previous_best) {
                record_broken_ = true;
                SpawnParticles(Cell{12, 12}, 50U, Rgb{252U, 211U, 77U}, 2U);
                TriggerFlash(Rgb{251U, 191U, 36U}, 600000U);
                TriggerShake(true, 500000U);
                PlayLevelUpSound();
            }
            app_.log().Info("snake: M18 food effects emitted from fixed pools");
        }
        if (outcome.level_up) {
            level_banner_us_ = 2000000U;
            TriggerFlash(ThemeForLevel(model_.level()).accent, 500000U);
            TriggerShake(true, 400000U);
            PlayLevelUpSound();
            app_.log().Info("snake: M18 level theme and upgrade feedback advanced");
        }
        if (outcome.collision) {
            PersistBestScore();
            StopAudio();
            PlayDieSound();
            TriggerFlash(Rgb{244U, 63U, 94U}, 500000U);
            TriggerShake(true, 400000U);
            screen_ = Screen::kGameOver;
            screen_button_.SetBounds(kRestartButtonRect);
            screen_button_.Reset();
            app_.log().Info("snake: game over with M18 statistics and feedback");
        }
    }
    Render();
}

void SnakeGame::OnTouch(const micropixel::TouchEvent& touch) {
    if (screen_ != Screen::kPlaying) {
        const micropixel::ui::ButtonUpdate update = screen_button_.OnTouch(touch);
        if (update.clicked) {
            if (screen_ == Screen::kMenu) {
                StartGame();
                Render();
            } else if (screen_ == Screen::kPaused) {
                TogglePause();
            } else if (screen_ == Screen::kGameOver) {
                StopAudio();
                StartGame();
                app_.log().Info("snake: restarted directly after game over");
                Render();
            }
        } else if (update.visual_changed) {
            Render();
        }
        return;
    }

    const micropixel::ui::ButtonUpdate pause_update = pause_touch_button_.OnTouch(touch);
    if (pause_update.handled) {
        if (pause_update.clicked) {
            TogglePause();
        }
        return;
    }
    const snake::gamekit::Gesture gesture = touch_gesture_.Update(touch);
    if (gesture.kind == snake::gamekit::GestureKind::kSwipe) {
        CommitSwipe(gesture.dx, gesture.dy);
    }
}

[[nodiscard]] uint64_t SnakeGame::MovementPeriodUs() const {
    return static_cast<uint64_t>(LeisurePeriodMs(model_.level())) * 1000U;
}

void SnakeGame::TogglePause() {
    if (screen_ == Screen::kPlaying) {
        screen_ = Screen::kPaused;
        screen_button_.SetBounds(kStartButtonRect);
        screen_button_.Reset();
        pause_touch_button_.Reset();
        StopAudio();
        app_.log().Info("snake: paused; logic clock frozen");
    } else if (screen_ == Screen::kPaused) {
        screen_ = Screen::kPlaying;
        screen_button_.Reset();
        pause_touch_button_.Reset();
        accumulated_us_ = 0U;
        StartBgm();
        app_.log().Info("snake: resumed without catch-up ticks");
    }
    Render();
}

void SnakeGame::CommitSwipe(int32_t dx, int32_t dy) {
    int32_t abs_x = AbsoluteValue(dx);
    int32_t abs_y = AbsoluteValue(dy);
    if (screen_ != Screen::kPlaying) {
        return;
    }
    Direction requested =
        abs_x > abs_y ? (dx > 0 ? Direction::kRight : Direction::kLeft) : (dy > 0 ? Direction::kDown : Direction::kUp);
    (void)model_.RequestDirection(requested);
}

void SnakeGame::PersistBestScore() {
    if (!best_dirty_) {
        return;
    }
    auto stored = app_.storage().SetU32("best", best_score_);
    if (stored.has_value()) {
        best_dirty_ = false;
        app_.log().Info("snake: best score committed");
    } else {
        app_.log().Info("snake: best score persistence deferred by KV quota");
    }
}

void SnakeGame::StartGame() {
    ResetGameModel();
    SnapshotBody();
    ResetBodySlotMapping();
    ResetEffects();
    accumulated_us_ = 0U;
    animation_time_us_ = 0U;
    level_banner_us_ = 0U;
    record_broken_ = false;
    logic_debt_logged_ = false;
    screen_ = Screen::kPlaying;
    screen_button_.Reset();
    pause_touch_button_.Reset();
    PlayStartSound();
    StartBgm();
    if (audio_available_) {
        app_.log().Info("snake: Audio 1.0 start cue and BGM scheduled");
    }
    app_.log().Info("snake: game started from menu");
}

void SnakeGame::ResetGameModel() {
    const uint32_t seed = app_.random().U32();
    model_.Reset(seed);
    effect_random_ = seed ^ 0xa5a5a5a5U;
}

}  // namespace snake
