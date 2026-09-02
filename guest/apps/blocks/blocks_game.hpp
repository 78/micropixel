#ifndef MICROPIXEL_BLOCKS_GAME_HPP
#define MICROPIXEL_BLOCKS_GAME_HPP

#include "apps/blocks/blocks_model.hpp"
#include "blocks_sfx_profiles.hpp"
#include "blocks_strings.hpp"
#include "sdk/ui/button.hpp"

namespace blocks {

struct ScheduledTone final {
    micropixel::Tone tone{};
    uint64_t delay_us{};
    bool active{};
};

class BlocksGame final {
   public:
    BlocksGame(micropixel::Application& app, micropixel::Renderer renderer, micropixel::RendererInfo renderer_info,
               micropixel::Audio audio, bool audio_available, uint32_t best_score);

    void OnTimer(const micropixel::TimerEvent& tick);
    void OnTouch(const micropixel::TouchEvent& touch);
    void Render();

   private:
    void StartNewGame();
    void EnterPause();
    void ResumeGame();
    void EnterGameOver();
    void HandleOutcome(const LockOutcome& outcome);
    void HandlePlayGesture(const micropixel::TouchEvent& touch);
    void ResetGesture();
    void InitializePlayfieldSurfaces();
    void SyncPlayfield();
    void InitializeScene();
    void RasterizeCell(uint32_t column, uint32_t row, uint8_t visual);
    void PutCellPixel(uint32_t x, uint32_t y, Rgb color);
    [[nodiscard]] uint8_t VisualCell(uint32_t column, uint32_t row) const;
    void RenderMiniPiece(micropixel::SceneUpdate& update, uint16_t first_instance, Tetromino type, int32_t center_x,
                         int32_t top, bool muted, bool visible);
    void RenderHeader(micropixel::SceneUpdate& update, const Theme& theme);
    void RenderSidebar(micropixel::SceneUpdate& update, const Theme& theme);
    void RenderStatusEffect(micropixel::SceneUpdate& update, const Theme& theme);
    void RenderOverlay(micropixel::SceneUpdate& update);

    [[nodiscard]] micropixel::Tone SynthTone(micropixel::Waveform waveform, uint32_t frequency_hz, uint32_t duration_ms,
                                             uint16_t volume_per_mille, uint16_t attack_ms = 4U,
                                             uint16_t release_ms = 30U) const;
    void EmitTone(const micropixel::Tone& tone);
    void QueueTone(const micropixel::Tone& tone, uint32_t delay_ms = 0U);
    void QueueProfile(const blocks_sfx::ToneSpec* tones, uint32_t count);
    void AdvanceAudio(uint64_t delta_us);
    void ClearAudioQueue();
    void PlayStartSound();
    void PlayMoveSound();
    void PlayRotateSound();
    void PlayHoldSound();
    void PlayLockSound(uint8_t drop_distance);
    void PlayLineSound(uint32_t lines, bool level_up);
    void PlayGameOverSound();

    micropixel::Application& app_;
    blocks_strings::Catalog strings_;
    micropixel::Renderer renderer_;
    micropixel::RendererInfo renderer_info_;
    micropixel::Scene scene_;
    micropixel::ContainerNode root_container_{};
    micropixel::SurfaceNode playfield_nodes_[4U]{};
    micropixel::RoundedRectNode sidebar_panels_[kSidebarPanelCount]{};
    micropixel::SpriteBatch mini_piece_batch_{};
    micropixel::SpriteBatch status_batch_{};
    micropixel::ui::FlexContainer hud_{};
    micropixel::LabelNode sidebar_labels_[6U]{};
    micropixel::LabelNode status_label_{};
    micropixel::ShapeNode overlay_node_{};
    micropixel::ui::TextButton action_button_{};
    micropixel::ui::FlexContainer game_over_panel_{};
    micropixel::Audio audio_;
    BlocksModel model_{};
    micropixel::StreamingTexture playfield_surfaces_[4U]{};
    ScheduledTone scheduled_tones_[8U]{};
    Screen screen_{Screen::kMenu};
    uint32_t best_score_{};
    uint32_t clear_rows_mask_{};
    uint32_t clear_points_{};
    uint64_t gravity_accumulated_us_{};
    uint64_t clear_effect_remaining_us_{};
    uint64_t gesture_started_us_{};
    GestureAxis gesture_axis_{GestureAxis::kUndecided};
    int32_t gesture_start_x_{};
    int32_t gesture_start_y_{};
    int32_t gesture_anchor_x_{};
    int32_t gesture_anchor_y_{};
    uint32_t gesture_touch_id_{};
    uint8_t visual_cells_[kBoardColumns * kBoardRows]{};
    alignas(4) uint8_t cell_pixels_[kCellPitch * kCellPitch * 2U]{};
    bool gesture_active_{};
    bool gesture_moved_{};
    bool gesture_started_in_pause_{};
    bool gesture_started_in_hold_{};
    bool audio_available_{};
    bool audio_error_logged_{};
    bool storage_error_logged_{};
    bool visual_cache_valid_{};
    bool scene_initialized_{};
};

}  // namespace blocks

#endif
