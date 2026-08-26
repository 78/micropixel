#ifndef MICROPIXEL_SNAKE_GAME_HPP
#define MICROPIXEL_SNAKE_GAME_HPP

#include "apps/snake/gamekit/cyclic_pool.hpp"
#include "apps/snake/gamekit/swipe_gesture.hpp"
#include "apps/snake/snake_model.hpp"
#include "snake_sfx_profiles.hpp"
#include "snake_strings.hpp"

namespace snake {

class SnakeGame final {
   public:
    SnakeGame(micropixel::Application& app, micropixel::Renderer renderer, micropixel::RendererInfo renderer_info,
              micropixel::Audio audio, bool audio_available, uint32_t best_score);

    void Render();

    void SetBoard(micropixel::Texture texture);

    void SetButtonTextures(micropixel::Texture start, micropixel::Texture restart);

    void SetBurstSheet(FoodType type, micropixel::Texture texture);

    void SetFoodSheet(FoodType type, micropixel::Texture texture);

    void OnTimer(const micropixel::TimerEvent& tick);

    void OnTouch(const micropixel::TouchEvent& touch);

   private:
    static micropixel::Rect CellRect(Cell cell, int32_t inset, int32_t board_x, int32_t board_y);

    [[nodiscard]] uint64_t MovementPeriodUs() const;

    [[nodiscard]] uint32_t MotionFractionQ8() const;

    micropixel::Rect InterpolatedSlotRect(uint32_t slot, uint32_t index, int32_t inset, int32_t board_x,
                                          int32_t board_y) const;

    void AppendPlaceholderRect(micropixel::Frame& commands) const;

    static void AppendPlaceholderText(micropixel::Frame& commands);

    void FillClippedRect(micropixel::Frame& commands, micropixel::Rect rect, micropixel::Color color) const;

    void RenderComboBar(micropixel::Frame& commands) const;

    void RenderComboFlame(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                          uint32_t slots) const;

    void RenderTrails(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                      uint32_t slots) const;

    void RenderFood(micropixel::Frame& commands, const Food& food, int32_t board_x, int32_t board_y,
                    uint32_t detail_slots) const;

    void RenderObstacles(micropixel::Frame& commands, int32_t board_x, int32_t board_y, uint32_t detail_slots) const;

    void RenderSnake(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                     uint32_t body_slots) const;

    void RenderFoodBurst(micropixel::Frame& commands, int32_t board_x, int32_t board_y) const;

    void RenderParticles(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                         uint32_t slots) const;

    void RenderFlash(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                     uint32_t slots) const;

    void RenderOverlayRect(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme) const;

    void RenderHeaderTexts(micropixel::Frame& commands, const Theme& theme) const;

    void RenderPopups(micropixel::Frame& commands, int32_t board_x, int32_t board_y, const Theme& theme) const;

    void RenderOverlayTexts(micropixel::Frame& commands, const Theme& theme) const;

    void SnapshotBody();

    void ResetBodySlotMapping();

    void AdvanceBodySlotMapping(Cell previous_head, uint32_t previous_length);

    static void AgeValue(uint32_t& age_us, uint32_t duration_us, bool& active, uint64_t delta_us);

    void AdvanceEffects(uint64_t delta_us);

    void SpawnTrail(Cell cell);

    void SpawnParticles(Cell origin, uint32_t count, Rgb color, uint32_t scale);

    void SpawnPopup(Cell cell, uint32_t points, Rgb color,
                    micropixel::SystemFont font = micropixel::SystemFont::kLarge);

    void TriggerFlash(Rgb color, uint64_t duration_us);

    void TriggerShake(bool heavy, uint64_t duration_us);

    void TriggerFoodEffects(Cell cell, const MoveOutcome& outcome);

    [[nodiscard]] int32_t ShakeComponent(bool x_axis) const;

    [[nodiscard]] int32_t ShakeX() const;

    [[nodiscard]] int32_t ShakeY() const;

    void ResetEffects();

    void TogglePause();

    void CommitSwipe(int32_t dx, int32_t dy);

    void PersistBestScore();

    static micropixel::Tone SynthTone(micropixel::Waveform waveform, uint32_t frequency_hz, uint32_t duration_ms,
                                      uint16_t volume_per_mille, uint16_t attack_ms = 5U, uint16_t release_ms = 20U);

    void NoteAudioError();

    void EmitTone(const micropixel::Tone& tone);

    void QueueTone(const micropixel::Tone& tone, uint32_t delay_ms);

    void QueueProfile(const snake_sfx::ToneSpec* tones, uint32_t count);

    void ClearScheduledTones();

    void StopAudio();

    void StartBgm();

    void AdvanceAudio(uint64_t delta_us);

    void PlayStartSound();

    void PlayFoodSound(FoodType type);

    void PlayLevelUpSound();

    void PlayDieSound();

    void StartGame();

    void ResetGameModel();

    micropixel::Application& app_;
    snake_strings::Catalog strings_;
    micropixel::Renderer renderer_;
    micropixel::RendererInfo renderer_info_;
    micropixel::Audio audio_;
    micropixel::Texture board_texture_{};
    micropixel::Texture start_button_texture_{};
    micropixel::Texture restart_button_texture_{};
    micropixel::Texture burst_sheets_[4U]{};
    micropixel::Texture food_sheets_[4U]{};
    SnakeModel model_{};
    Cell burst_cell_{};
    Cell previous_body_[kMaxLength]{};
    Cell body_slot_previous_[kMaxLength]{};
    snake::gamekit::CyclicPool<Particle, kParticlePoolSize> particles_{};
    snake::gamekit::CyclicPool<Trail, kTrailPoolSize> trails_{};
    snake::gamekit::CyclicPool<Popup, kPopupPoolSize> popups_{};
    ScheduledTone scheduled_tones_[12]{};
    uint64_t accumulated_us_{};
    uint64_t animation_time_us_{};
    uint32_t best_score_{};
    uint64_t level_banner_us_{};
    uint64_t flash_remaining_us_{};
    uint64_t flash_duration_us_{};
    uint64_t shake_remaining_us_{};
    uint64_t shake_duration_us_{};
    uint64_t burst_remaining_us_{};
    uint32_t previous_length_{};
    uint32_t body_slot_head_{};
    uint32_t body_slot_length_{};
    uint64_t bgm_remaining_us_{};
    uint32_t bgm_note_index_{};
    uint32_t effect_random_{kRandomSeed ^ 0xa5a5a5a5U};
    uint8_t shake_capture_delay_frames_{};
    Rgb flash_color_{};
    FoodType burst_type_{FoodType::kNormal};
    bool best_dirty_{};
    bool record_broken_{};
    bool logic_debt_logged_{};
    bool shake_heavy_{};
    bool audio_available_{};
    bool bgm_playing_{};
    bool audio_error_logged_{};
    Screen screen_{Screen::kMenu};
    micropixel::ui::Button screen_button_{};
    micropixel::ui::Button pause_touch_button_{};
    snake::gamekit::SwipeGesture touch_gesture_{};
};

}  // namespace snake

#endif
