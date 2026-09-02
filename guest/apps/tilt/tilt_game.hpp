#ifndef MICROPIXEL_TILT_GAME_HPP
#define MICROPIXEL_TILT_GAME_HPP

#include "apps/tilt/tilt_input.hpp"
#include "apps/tilt/tilt_model.hpp"
#include "tilt_sfx_profiles.hpp"

namespace tilt {

inline constexpr uint32_t kProgressSchemaVersion = 2U;

struct ProgressData final {
    uint32_t schema_version{kProgressSchemaVersion};
    uint32_t level_count{kLevelCount};
    uint32_t unlocked_level_index{};
    uint32_t best_times_ms[kLevelCount]{};
    uint8_t best_ratings[kLevelCount]{};
};

static_assert(sizeof(ProgressData) <= micropixel::KVStore::kMaximumValueBytes,
              "tilt progress must fit in one atomic storage value");

class TiltGame final {
   public:
    TiltGame(micropixel::Application& app, micropixel::Renderer renderer, micropixel::RendererInfo renderer_info,
             micropixel::Texture board_base_texture, micropixel::Texture board_frame_texture,
             micropixel::Texture board_tile_texture, uint32_t board_tile_frame_pixels,
             micropixel::Texture object_texture, uint32_t object_frame_pixels, micropixel::Texture fan_texture,
             uint32_t fan_frame_pixels, micropixel::Texture mechanic_texture, uint32_t mechanic_frame_pixels,
             micropixel::Texture title_texture, micropixel::Texture hud_texture, uint32_t hud_frame_pixels,
             micropixel::Audio audio, bool audio_available, const ProgressData& progress);

    void Render();
    void OnTimer(const micropixel::TimerEvent& tick);
    void OnTouch(const micropixel::TouchEvent& touch);
    void OnResume();

   private:
    struct Trail final {
        PointF position{};
        uint32_t age_us{};
        bool active{};
    };

    struct Particle final {
        PointF position{};
        Vec2 velocity{};
        uint32_t age_us{};
        uint32_t duration_us{};
        bool active{};
    };

    struct ScheduledTone final {
        micropixel::Tone tone{};
        uint64_t delay_us{};
        bool active{};
    };

    void InitializeScene();
    void StartCalibration(bool reset_model);
    void CompleteCalibration();
    void SelectLevel(uint32_t level_index);
    void Pause();
    void HandleOutcome(const ModelOutcome& outcome);
    void CompleteCourse();
    void PersistProgress();
    void ResetEffects();
    void AdvanceEffects(uint64_t delta_us);
    void SpawnTrail();
    void SpawnParticles(PointF origin, uint32_t count);
    void RenderObjects(micropixel::SceneUpdate& update);
    void RenderEffects(micropixel::SceneUpdate& update);
    void RenderHud(micropixel::SceneUpdate& update);
    void RenderModal(micropixel::SceneUpdate& update);
    [[nodiscard]] micropixel::Rect FrameSource(ObjectFrame frame) const;
    [[nodiscard]] micropixel::Rect BoardTileFrameSource(BoardTileFrame frame) const;
    [[nodiscard]] micropixel::Rect FanFrameSource(uint32_t frame) const;
    [[nodiscard]] micropixel::Rect MechanicFrameSource(MechanicFrame frame) const;
    [[nodiscard]] micropixel::Rect HudFrameSource(uint32_t frame) const;
    [[nodiscard]] static micropixel::Rect ObjectDestination(PointF center);
    [[nodiscard]] static micropixel::Rect BoardWallDestination(WallRect block);
    [[nodiscard]] static micropixel::Rect BoardIceDestination(RectFeature rect);
    [[nodiscard]] static micropixel::Rect BoardAirflowDestination(FanFeature fan);
    [[nodiscard]] static BoardTileFrame AirflowFrame(FanFeature fan);
    [[nodiscard]] static micropixel::Rect MechanicDestination(WallRect rect);
    static void FormatTime(uint64_t elapsed_us, char (&output)[6]);

    [[nodiscard]] micropixel::Tone SynthTone(micropixel::Waveform waveform, uint32_t frequency_hz, uint32_t duration_ms,
                                             uint16_t volume_per_mille, uint16_t attack_ms, uint16_t release_ms) const;
    void EmitTone(const micropixel::Tone& tone);
    void QueueTone(const micropixel::Tone& tone, uint32_t delay_ms);
    void QueueProfile(const tilt_sfx::ToneSpec* tones, uint32_t count);
    void AdvanceAudio(uint64_t delta_us);
    void ClearAudioQueue();
    void PlayStartSound();
    void PlayWallSound();
    void PlayBumperSound();
    void PlayStarSound();
    void PlayFallSound();
    void PlayCompleteSound();

    micropixel::Application& app_;
    micropixel::Renderer renderer_;
    micropixel::RendererInfo renderer_info_;
    micropixel::Scene scene_;
    micropixel::Texture board_base_texture_{};
    micropixel::Texture board_frame_texture_{};
    micropixel::Texture board_tile_texture_{};
    micropixel::Texture object_texture_{};
    micropixel::Texture fan_texture_{};
    micropixel::Texture mechanic_texture_{};
    micropixel::Texture title_texture_{};
    micropixel::Texture hud_texture_{};
    uint32_t object_frame_pixels_{};
    uint32_t board_tile_frame_pixels_{};
    uint32_t fan_frame_pixels_{};
    uint32_t mechanic_frame_pixels_{};
    uint32_t hud_frame_pixels_{};
    micropixel::ContainerNode root_container_{};
    micropixel::ContainerNode game_container_{};
    micropixel::ContainerNode hud_container_{};
    micropixel::ContainerNode modal_container_{};
    micropixel::SpriteNode board_base_node_{};
    micropixel::SpriteBatch board_tile_batch_{};
    micropixel::SpriteNode board_frame_node_{};
    micropixel::SpriteBatch object_batch_{};
    micropixel::SpriteBatch fan_batch_{};
    micropixel::SpriteNode moving_wall_node_{};
    micropixel::SpriteNode gate_node_{};
    micropixel::SpriteNode pressure_gate_node_{};
    micropixel::SpriteNode pressure_plate_node_{};
    micropixel::SpriteBatch portal_batch_{};
    micropixel::SpriteNode goal_node_{};
    micropixel::SpriteBatch trail_batch_{};
    micropixel::SpriteNode marble_node_{};
    micropixel::SpriteBatch particle_batch_{};
    micropixel::SpriteNode title_node_{};
    micropixel::RoundedRectNode star_panel_{};
    micropixel::SpriteBatch hud_star_batch_{};
    micropixel::RoundedRectNode tilt_panel_{};
    micropixel::SpriteNode tilt_icon_node_{};
    micropixel::LabelNode level_label_{};
    micropixel::LabelNode time_label_{};
    micropixel::ShapeNode modal_dim_{};
    micropixel::LabelNode modal_title_{};
    micropixel::LabelNode modal_subtitle_{};
    micropixel::ui::TextButton action_button_{};
    micropixel::ui::TextButton secondary_button_{};
    micropixel::ui::TextButton previous_level_button_{};
    micropixel::ui::TextButton next_level_button_{};
    micropixel::ui::Button pause_button_{kPauseTouchRect};
    micropixel::Audio audio_;
    TiltInput input_{};
    TiltModel model_{};
    Trail trails_[kTrailCapacity]{};
    Particle particles_[kParticleCapacity]{};
    ScheduledTone scheduled_tones_[8U]{};
    uint64_t animation_time_us_{};
    uint64_t trail_accumulated_us_{};
    uint64_t wall_sound_cooldown_us_{};
    uint64_t bumper_flash_us_{};
    uint32_t trail_cursor_{};
    uint32_t particle_cursor_{};
    ProgressData progress_{};
    uint32_t completed_rating_{};
    uint32_t selected_level_index_{};
    uint32_t rendered_level_index_{UINT32_MAX};
    bool audio_available_{};
    bool audio_error_logged_{};
    bool storage_error_logged_{};
    bool scene_initialized_{};
    Screen screen_{Screen::kMenu};
};

}  // namespace tilt

#endif
