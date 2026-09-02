#include "apps/tilt/tilt_game.hpp"

#include "apps/tilt/tilt_progress_keys.hpp"

namespace tilt {

TiltGame::TiltGame(micropixel::Application& app, micropixel::Renderer renderer, micropixel::RendererInfo renderer_info,
                   micropixel::Texture board_base_texture, micropixel::Texture board_frame_texture,
                   micropixel::Texture board_tile_texture, uint32_t board_tile_frame_pixels,
                   micropixel::Texture object_texture, uint32_t object_frame_pixels, micropixel::Texture fan_texture,
                   uint32_t fan_frame_pixels, micropixel::Texture mechanic_texture, uint32_t mechanic_frame_pixels,
                   micropixel::Texture title_texture, micropixel::Texture hud_texture, uint32_t hud_frame_pixels,
                   micropixel::Audio audio, bool audio_available, const ProgressData& progress)
    : app_(app),
      renderer_(renderer),
      renderer_info_(renderer_info),
      scene_(renderer.CreateScene(micropixel::Color::Rgb(5U, 8U, 13U))),
      board_base_texture_(static_cast<micropixel::Texture&&>(board_base_texture)),
      board_frame_texture_(static_cast<micropixel::Texture&&>(board_frame_texture)),
      board_tile_texture_(static_cast<micropixel::Texture&&>(board_tile_texture)),
      object_texture_(static_cast<micropixel::Texture&&>(object_texture)),
      fan_texture_(static_cast<micropixel::Texture&&>(fan_texture)),
      mechanic_texture_(static_cast<micropixel::Texture&&>(mechanic_texture)),
      title_texture_(static_cast<micropixel::Texture&&>(title_texture)),
      hud_texture_(static_cast<micropixel::Texture&&>(hud_texture)),
      object_frame_pixels_(object_frame_pixels),
      board_tile_frame_pixels_(board_tile_frame_pixels),
      fan_frame_pixels_(fan_frame_pixels),
      mechanic_frame_pixels_(mechanic_frame_pixels),
      hud_frame_pixels_(hud_frame_pixels),
      audio_(audio),
      progress_(progress),
      audio_available_(audio_available) {
    progress_.schema_version = kProgressSchemaVersion;
    progress_.level_count = kLevelCount;
    if (progress_.unlocked_level_index >= kLevelCount) {
        progress_.unlocked_level_index = 0U;
    }
    selected_level_index_ = progress_.unlocked_level_index;
    model_.Reset(selected_level_index_);
    const bool sensor_available = input_.Initialize(app_);
    screen_ = sensor_available ? Screen::kMenu : Screen::kUnsupported;
}

void TiltGame::StartCalibration(bool reset_model) {
    if (reset_model) {
        model_.Reset(selected_level_index_);
    }
    input_.Recalibrate();
    screen_ = Screen::kCalibrating;
    ResetEffects();
    Render();
}

void TiltGame::CompleteCalibration() {
    screen_ = Screen::kPlaying;
    PlayStartSound();
    Render();
}

void TiltGame::SelectLevel(uint32_t level_index) {
    if (level_index > progress_.unlocked_level_index || level_index >= kLevelCount) {
        return;
    }
    selected_level_index_ = level_index;
    model_.Reset(selected_level_index_);
    ResetEffects();
    Render();
}

void TiltGame::Pause() {
    if (screen_ != Screen::kPlaying) {
        return;
    }
    screen_ = Screen::kPaused;
    pause_button_.Reset();
    ClearAudioQueue();
    Render();
}

void TiltGame::CompleteCourse() {
    screen_ = Screen::kComplete;
    const uint32_t completed_level = model_.level_index();
    const uint32_t elapsed_ms = static_cast<uint32_t>(model_.elapsed_us() / 1000U);
    completed_rating_ = model_.mastery_rating();
    bool progress_changed = false;
    if (progress_.best_times_ms[completed_level] == 0U || elapsed_ms < progress_.best_times_ms[completed_level]) {
        progress_.best_times_ms[completed_level] = elapsed_ms;
        progress_changed = true;
    }
    if (completed_rating_ > progress_.best_ratings[completed_level]) {
        progress_.best_ratings[completed_level] = static_cast<uint8_t>(completed_rating_);
        progress_changed = true;
    }
    if (completed_level + 1U < kLevelCount) {
        selected_level_index_ = completed_level + 1U;
        if (selected_level_index_ > progress_.unlocked_level_index) {
            progress_.unlocked_level_index = selected_level_index_;
            progress_changed = true;
        }
    } else {
        selected_level_index_ = completed_level;
    }
    if (progress_changed) {
        PersistProgress();
    }
    SpawnParticles(model_.ball(), kParticleCapacity);
    PlayCompleteSound();
}

void TiltGame::PersistProgress() {
    // Keep the highest unlocked level in the original key as a durable fallback. Older
    // releases used enough per-level keys to exhaust the namespace key quota, which can
    // prevent progress_v2 from being created during migration even though this key can
    // still be updated in place.
    const auto unlocked_stored = app_.storage().SetU32("unlocked_level", progress_.unlocked_level_index);
    bool details_saved = false;
    bool reclaiming_legacy_keys = false;
    {
        const auto details_stored = app_.storage().SetBytes("progress_v2", reinterpret_cast<const uint8_t*>(&progress_),
                                                            static_cast<uint32_t>(sizeof(progress_)));
        details_saved = details_stored.has_value();
        reclaiming_legacy_keys =
            !details_saved && details_stored.error().code() == micropixel::ErrorCode::kResourceExhausted;
    }

    // Once the new snapshot is durable, old per-level keys are redundant. If the
    // namespace is already full, remove them one at a time and retry the snapshot.
    // The Host write limiter bounds how much cleanup one completion can perform;
    // later completions resume naturally at the first remaining key.
    bool cleanup_stopped = false;
    auto reclaim_key = [&](const char* key) {
        if ((!details_saved && !reclaiming_legacy_keys) || cleanup_stopped) {
            return;
        }
        const auto removed = app_.storage().Remove(key);
        if (!removed.has_value()) {
            if (removed.error().code() != micropixel::ErrorCode::kNotFound) {
                cleanup_stopped = true;
            }
            return;
        }
        if (!details_saved) {
            const auto retry = app_.storage().SetBytes("progress_v2", reinterpret_cast<const uint8_t*>(&progress_),
                                                       static_cast<uint32_t>(sizeof(progress_)));
            details_saved = retry.has_value();
            reclaiming_legacy_keys =
                !details_saved && retry.error().code() == micropixel::ErrorCode::kResourceExhausted;
        }
    };
    reclaim_key("best_ms");
    for (uint32_t index = 0U; index < kLegacyLevelCount; ++index) {
        reclaim_key(kLegacyBestTimeKeys[index]);
        reclaim_key(kLegacyBestRatingKeys[index]);
    }

    if ((!unlocked_stored.has_value() || !details_saved) && !storage_error_logged_) {
        storage_error_logged_ = true;
        if (unlocked_stored.has_value()) {
            app_.log().Info("tilt: detailed progress persistence failed; unlocked level saved");
        } else {
            app_.log().Info("tilt: unlocked level persistence failed; gameplay continues");
        }
    }
}

void TiltGame::HandleOutcome(const ModelOutcome& outcome) {
    if (outcome.wall_hit && wall_sound_cooldown_us_ == 0U) {
        wall_sound_cooldown_us_ = 90000U;
        PlayWallSound();
    }
    if (outcome.bumper_hit) {
        bumper_flash_us_ = 180000U;
        PlayBumperSound();
    }
    if (outcome.star_collected) {
        SpawnParticles(model_.ball(), 10U);
        PlayStarSound();
    }
    if (outcome.pressure_plate_activated) {
        SpawnParticles(model_.ball(), 8U);
        PlayBumperSound();
    }
    if (outcome.teleported) {
        SpawnParticles(model_.ball(), 12U);
    }
    if (outcome.fell) {
        ResetEffects();
        PlayFallSound();
    }
    if (outcome.completed) {
        CompleteCourse();
    }
}

void TiltGame::OnTimer(const micropixel::TimerEvent& tick) {
    const uint64_t delta_us = tick.delta().count_microseconds();
    AdvanceAudio(delta_us);
    if (screen_ != Screen::kPaused) {
        animation_time_us_ += delta_us;
    }
    if (wall_sound_cooldown_us_ != 0U) {
        wall_sound_cooldown_us_ = delta_us >= wall_sound_cooldown_us_ ? 0U : wall_sound_cooldown_us_ - delta_us;
    }
    if (bumper_flash_us_ != 0U) {
        bumper_flash_us_ = delta_us >= bumper_flash_us_ ? 0U : bumper_flash_us_ - delta_us;
    }
    (void)input_.Sample();
    if (screen_ == Screen::kCalibrating) {
        if (input_.calibrated()) {
            CompleteCalibration();
        } else {
            Render();
        }
        return;
    }
    if (screen_ != Screen::kPlaying) {
        if (screen_ == Screen::kComplete) {
            AdvanceEffects(delta_us);
            Render();
        }
        return;
    }
    HandleOutcome(model_.Advance(delta_us, input_.tilt()));
    AdvanceEffects(delta_us);
    trail_accumulated_us_ += delta_us;
    if (trail_accumulated_us_ >= 50000U) {
        trail_accumulated_us_ %= 50000U;
        SpawnTrail();
    }
    Render();
}

void TiltGame::OnTouch(const micropixel::TouchEvent& touch) {
    if (screen_ == Screen::kPlaying) {
        if (pause_button_.OnTouch(touch).clicked) {
            Pause();
        }
        return;
    }
    if (screen_ != Screen::kMenu && screen_ != Screen::kPaused && screen_ != Screen::kComplete) {
        return;
    }

    const micropixel::ui::ButtonUpdate action = action_button_.OnTouch(touch);
    if (action.clicked) {
        if (screen_ == Screen::kPaused) {
            StartCalibration(false);
        } else {
            StartCalibration(true);
        }
        return;
    }
    const micropixel::ui::ButtonUpdate secondary =
        secondary_button_.visible() ? secondary_button_.OnTouch(touch) : micropixel::ui::ButtonUpdate{};
    if (secondary.clicked) {
        if (screen_ == Screen::kPaused) {
            StartCalibration(true);
        } else {
            selected_level_index_ = 0U;
            model_.Reset(selected_level_index_);
            StartCalibration(true);
        }
        return;
    }
    if (screen_ == Screen::kMenu) {
        const micropixel::ui::ButtonUpdate previous = previous_level_button_.OnTouch(touch);
        if (previous.clicked && selected_level_index_ != 0U) {
            SelectLevel(selected_level_index_ - 1U);
            return;
        }
        const micropixel::ui::ButtonUpdate next = next_level_button_.OnTouch(touch);
        if (next.clicked && selected_level_index_ < progress_.unlocked_level_index) {
            SelectLevel(selected_level_index_ + 1U);
            return;
        }
        if (previous.redraw() || next.redraw()) {
            Render();
        }
    }
    if (action.redraw() || secondary.redraw()) {
        Render();
    }
}

void TiltGame::OnResume() {
    if (screen_ == Screen::kPlaying) {
        StartCalibration(false);
    } else {
        Render();
    }
}

void TiltGame::ResetEffects() {
    for (Trail& trail : trails_) {
        trail.active = false;
    }
    for (Particle& particle : particles_) {
        particle.active = false;
    }
    trail_cursor_ = 0U;
    particle_cursor_ = 0U;
    trail_accumulated_us_ = 0U;
}

void TiltGame::SpawnTrail() {
    Trail& trail = trails_[trail_cursor_++ % kTrailCapacity];
    trail.position = model_.ball();
    trail.age_us = 0U;
    trail.active = true;
}

void TiltGame::SpawnParticles(PointF origin, uint32_t count) {
    constexpr Vec2 velocities[] = {
        {-95.0F, -135.0F}, {-35.0F, -165.0F}, {45.0F, -155.0F}, {105.0F, -115.0F}, {145.0F, -20.0F}, {95.0F, 90.0F},
        {20.0F, 145.0F},   {-75.0F, 110.0F},  {-145.0F, 25.0F}, {-125.0F, -70.0F}, {55.0F, 70.0F},   {-45.0F, 55.0F},
    };
    const uint32_t velocity_count = sizeof(velocities) / sizeof(velocities[0]);
    count = count > kParticleCapacity ? kParticleCapacity : count;
    for (uint32_t index = 0U; index < count; ++index) {
        Particle& particle = particles_[particle_cursor_++ % kParticleCapacity];
        particle.position = origin;
        particle.velocity = velocities[index % velocity_count];
        particle.age_us = 0U;
        particle.duration_us = 520000U;
        particle.active = true;
    }
}

void TiltGame::AdvanceEffects(uint64_t delta_us) {
    for (Trail& trail : trails_) {
        if (!trail.active) {
            continue;
        }
        trail.age_us += static_cast<uint32_t>(delta_us > UINT32_MAX ? UINT32_MAX : delta_us);
        trail.active = trail.age_us < 420000U;
    }
    const float seconds = static_cast<float>(delta_us) / 1000000.0F;
    for (Particle& particle : particles_) {
        if (!particle.active) {
            continue;
        }
        particle.age_us += static_cast<uint32_t>(delta_us > UINT32_MAX ? UINT32_MAX : delta_us);
        if (particle.age_us >= particle.duration_us) {
            particle.active = false;
            continue;
        }
        particle.position.x += particle.velocity.x * seconds;
        particle.position.y += particle.velocity.y * seconds;
        particle.velocity.y += 240.0F * seconds;
    }
}

}  // namespace tilt
