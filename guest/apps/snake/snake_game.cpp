#include "apps/snake/snake_game.hpp"

namespace snake {

namespace {

constexpr uint32_t kSwipeThresholdPhysicalPixels = 50U;

}  // namespace

SnakeGame::SnakeGame(micropixel::Application& app, micropixel::Renderer renderer,
                     micropixel::RendererInfo renderer_info, micropixel::Audio audio, bool audio_available,
                     uint32_t best_score)
    : app_(app),
      strings_(snake_strings::ForLocale(app.localization().CurrentLocale())),
      renderer_(renderer),
      renderer_info_(renderer_info),
      scene_(renderer.CreateScene(micropixel::Color::Rgb(5U, 5U, 5U))),
      audio_(audio),
      best_score_(best_score),
      audio_available_(audio_available),
      touch_gesture_{gamekit::ScalePhysicalThreshold(kSwipeThresholdPhysicalPixels, renderer_info.width(),
                                                     renderer_info.physical_width()),
                     gamekit::ScalePhysicalThreshold(kSwipeThresholdPhysicalPixels, renderer_info.height(),
                                                     renderer_info.physical_height())} {
    pause_touch_button_.SetBounds(kPauseTouchRect);
    ResetGameModel();
    ResetBodySlotMapping();
    app_.log().Info("snake: M18 menu ready");
    app_.log().Info("snake: renderer state translation enabled");
}

void SnakeGame::OnTimer(const micropixel::TimerEvent& tick) {
    uint64_t delta_us = tick.delta().count_microseconds();
    if (benchmark_) {
        // Timer delivery jitter: a 60 Hz ticker should never be later than
        // one and a half periods unless the Guest task was held somewhere.
        if (delta_us > bench_.tick_max_us) {
            bench_.tick_max_us = delta_us;
        }
        if (delta_us > kRenderTargetPeriodUs + kRenderTargetPeriodUs / 2U) {
            ++bench_.late_ticks;
        }
    }
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
        if (benchmark_) {
            BenchmarkSteer();
        }
        Cell previous_head = model_.body()[0];
        uint32_t previous_length = model_.length();
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
        if (outcome.collision && benchmark_) {
            // Keep the board animating: restart in place instead of showing
            // the GameOver screen.
            ++bench_.deaths;
            StartGame();
        } else if (outcome.collision) {
            PersistBestScore();
            StopAudio();
            PlayDieSound();
            TriggerFlash(Rgb{244U, 63U, 94U}, 500000U);
            TriggerShake(true, 400000U);
            screen_ = Screen::kGameOver;
            app_.log().Info("snake: game over with M18 statistics and feedback");
        }
    }
    Render();
}

void SnakeGame::OnTouch(const micropixel::TouchEvent& touch) {
    if (screen_ != Screen::kPlaying) {
        auto& button = screen_ == Screen::kGameOver ? game_over_panel_.text_button(0U) : action_button_;
        const micropixel::ui::ButtonUpdate update = button.OnTouch(touch);
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

    const micropixel::TouchEvent local_touch = touch.WithPosition(
        {touch.x() - ContentOffsetX(renderer_info_.width()), touch.y() - ContentOffsetY(renderer_info_.height())});
    const micropixel::ui::ButtonUpdate pause_update = pause_touch_button_.OnTouch(local_touch);
    if (pause_update.handled) {
        if (pause_update.clicked) {
            TogglePause();
        }
        return;
    }
    const snake::gamekit::Gesture gesture = touch_gesture_.Update(local_touch);
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
        pause_touch_button_.Reset();
        StopAudio();
        app_.log().Info("snake: paused; logic clock frozen");
    } else if (screen_ == Screen::kPaused) {
        screen_ = Screen::kPlaying;
        pause_touch_button_.Reset();
        accumulated_us_ = 0U;
        PlayStartSound();
        StartBgm();
        app_.log().Info("snake: resumed with start cue and without catch-up ticks");
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
    ResetBodySlotMapping();
    ResetEffects();
    accumulated_us_ = 0U;
    animation_time_us_ = 0U;
    level_banner_us_ = 0U;
    record_broken_ = false;
    logic_debt_logged_ = false;
    screen_ = Screen::kPlaying;
    pause_touch_button_.Reset();
    PlayStartSound();
    StartBgm();
    if (audio_available_) {
        app_.log().Info("snake: Audio 1.1 start cue and BGM scheduled");
    }
    app_.log().Info("snake: game started from menu");
}

void SnakeGame::ResetGameModel() {
    const uint32_t seed = app_.random().U32();
    model_.Reset(seed);
    effect_random_ = seed ^ 0xa5a5a5a5U;
}

void SnakeGame::EnableBenchmark(bool bgm_enabled) {
    benchmark_ = true;
    bgm_enabled_ = bgm_enabled;
    bench_ = {};
    StartGame();
    app_.log().Info(bgm_enabled ? "snake-bench: enabled (autopilot, restart on collision)"
                                : "snake-bench: enabled (autopilot, restart on collision, no BGM)");
}

bool SnakeGame::CellBlocked(Cell cell) const {
    if (cell.x < 0 || cell.y < 0 || cell.x >= static_cast<int16_t>(kColumns) || cell.y >= static_cast<int16_t>(kRows)) {
        return true;
    }
    const Cell* body = model_.body();
    // The tail cell is vacated by the same Move, so it is a legal target.
    const uint32_t occupied = model_.length() > 1U ? model_.length() - 1U : model_.length();
    for (uint32_t index = 0U; index < occupied; ++index) {
        if (SameCell(cell, body[index])) {
            return true;
        }
    }
    const Cell* obstacles = model_.obstacles();
    for (uint32_t index = 0U; index < model_.obstacle_count(); ++index) {
        if (SameCell(cell, obstacles[index])) {
            return true;
        }
    }
    return false;
}

void SnakeGame::BenchmarkSteer() {
    // Greedy autopilot: head for the food along the longer axis first and
    // fall back to any non-lethal turn. Dead ends still happen; they end in a
    // collision that the benchmark turns into an immediate restart.
    const Cell head = model_.body()[0];
    const Cell food = model_.food().cell;
    const int32_t dx = static_cast<int32_t>(food.x) - head.x;
    const int32_t dy = static_cast<int32_t>(food.y) - head.y;
    const Direction horizontal = dx > 0 ? Direction::kRight : Direction::kLeft;
    const Direction vertical = dy > 0 ? Direction::kDown : Direction::kUp;
    Direction candidates[4];
    if (AbsoluteValue(dx) >= AbsoluteValue(dy)) {
        candidates[0] = horizontal;
        candidates[1] = vertical;
    } else {
        candidates[0] = vertical;
        candidates[1] = horizontal;
    }
    candidates[2] = dx > 0 ? Direction::kLeft : Direction::kRight;
    candidates[3] = dy > 0 ? Direction::kUp : Direction::kDown;
    const Direction current = model_.direction();
    for (Direction candidate : candidates) {
        if (Opposite(current, candidate) || CellBlocked(StepCell(head, candidate))) {
            continue;
        }
        if (candidate != current) {
            (void)model_.RequestDirection(candidate);
        }
        return;
    }
}

void SnakeGame::BenchmarkRecordRender(uint64_t before_us, uint64_t after_us) {
    if (bench_.window_start_us == 0U) {
        bench_.window_start_us = before_us;
    }
    const uint64_t render_us = after_us - before_us;
    bench_.render_us_accum += render_us;
    if (render_us > bench_.render_max_us) {
        bench_.render_max_us = render_us;
    }
    if (++bench_.frames < 120U) {
        return;
    }
    const uint64_t elapsed_us = after_us - bench_.window_start_us;
    micropixel::FixedString<192U> msg;
    msg.Append("snake-bench: frames=");
    msg.AppendUint(bench_.frames);
    msg.Append(" elapsed_ms=");
    msg.AppendUint(static_cast<uint32_t>(elapsed_us / 1000U));
    msg.Append(" fps_x100=");
    msg.AppendUint(elapsed_us == 0U ? 0U : static_cast<uint32_t>(bench_.frames * 100000000ULL / elapsed_us));
    msg.Append(" render_avg_us=");
    msg.AppendUint(static_cast<uint32_t>(bench_.render_us_accum / bench_.frames));
    msg.Append(" render_max_us=");
    msg.AppendUint(static_cast<uint32_t>(bench_.render_max_us));
    msg.Append(" tick_max_us=");
    msg.AppendUint(static_cast<uint32_t>(bench_.tick_max_us));
    msg.Append(" late_ticks=");
    msg.AppendUint(bench_.late_ticks);
    msg.Append(" deaths=");
    msg.AppendUint(bench_.deaths);
    msg.Append(" len=");
    msg.AppendUint(model_.length());
    msg.Append(" level=");
    msg.AppendUint(model_.level());
    app_.log().Info(msg.c_str());
    bench_.frames = 0U;
    bench_.window_start_us = after_us;
    bench_.render_us_accum = 0U;
    bench_.render_max_us = 0U;
    bench_.tick_max_us = 0U;
    bench_.late_ticks = 0U;
}

}  // namespace snake
