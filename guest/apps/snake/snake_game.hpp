#ifndef MICROPIXEL_SNAKE_GAME_HPP
#define MICROPIXEL_SNAKE_GAME_HPP

#include "apps/snake/gamekit/cyclic_pool.hpp"
#include "apps/snake/gamekit/swipe_gesture.hpp"
#include "apps/snake/snake_model.hpp"
#include "snake_sfx_profiles.hpp"

namespace snake {

class SnakeGame final {
   public:
    SnakeGame(micropixel::Application& app, micropixel::Graphics graphics, micropixel::GraphicsInfo graphics_info,
              micropixel::Audio audio, bool audio_available, uint32_t best_score);

    void Render();

    void set_board(micropixel::Bitmap bitmap);

    void set_button_bitmaps(micropixel::Bitmap start, micropixel::Bitmap restart);

    void set_burst_sheet(FoodType type, micropixel::Bitmap bitmap);

    void set_food_sheet(FoodType type, micropixel::Bitmap bitmap);

    void OnTimer(const micropixel::TimerEvent& tick);

    void OnTouch(const micropixel::TouchEvent& touch);

   private:
    static micropixel::Rect CellRect(Cell cell, int32_t inset, int32_t board_x, int32_t board_y);

    [[nodiscard]] uint64_t MovementPeriodUs() const;

    [[nodiscard]] uint32_t MotionFractionQ8() const;

    micropixel::Rect InterpolatedSlotRect(uint32_t slot, uint32_t index, int32_t inset, int32_t board_x,
                                          int32_t board_y) const;

    void AppendPlaceholderRect(micropixel::CommandBuffer& commands) const;

    static void AppendPlaceholderText(micropixel::CommandBuffer& commands);

    void FillClippedRect(micropixel::CommandBuffer& commands, micropixel::Rect rect, micropixel::Color color) const;

    void RenderComboBar(micropixel::CommandBuffer& commands) const;

    void RenderComboFlame(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                          uint32_t slots) const;

    void RenderTrails(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                      uint32_t slots) const;

    void RenderFood(micropixel::CommandBuffer& commands, const Food& food, int32_t board_x, int32_t board_y,
                    uint32_t detail_slots) const;

    void RenderObstacles(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y,
                         uint32_t detail_slots) const;

    void RenderSnake(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                     uint32_t body_slots) const;

    void RenderFoodBurst(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y) const;

    void RenderParticles(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                         uint32_t slots) const;

    void RenderFlash(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y, const Theme& theme,
                     uint32_t slots) const;

    void RenderOverlayRect(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y,
                           const Theme& theme) const;

    void RenderHeaderTexts(micropixel::CommandBuffer& commands, const Theme& theme) const;

    void RenderPopups(micropixel::CommandBuffer& commands, int32_t board_x, int32_t board_y, const Theme& theme) const;

    void RenderOverlayTexts(micropixel::CommandBuffer& commands, const Theme& theme) const;

    void SnapshotBody();

    void ResetBodySlotMapping();

    void AdvanceBodySlotMapping(Cell previous_head, uint32_t previous_length);

    static void AgeValue(uint32_t& age_us, uint32_t duration_us, bool& active, uint64_t delta_us);

    void AdvanceEffects(uint64_t delta_us);

    void SpawnTrail(Cell cell);

    void SpawnParticles(Cell origin, uint32_t count, Rgb color, uint32_t scale);

    void SpawnPopup(Cell cell, uint32_t points, Rgb color, uint8_t font_size_px = 24U);

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
    micropixel::Graphics graphics_;
    micropixel::GraphicsInfo graphics_info_;
    micropixel::Audio audio_;
    micropixel::Bitmap board_bitmap_{};
    micropixel::Bitmap start_button_bitmap_{};
    micropixel::Bitmap restart_button_bitmap_{};
    micropixel::Bitmap burst_sheets_[4U]{};
    micropixel::Bitmap food_sheets_[4U]{};
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
    bool surface_translation_available_{};
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
