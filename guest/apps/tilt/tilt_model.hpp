#ifndef MICROPIXEL_TILT_MODEL_HPP
#define MICROPIXEL_TILT_MODEL_HPP

#if defined(MICROPIXEL_TILT_LEVEL_DATA_HEADER)
#include MICROPIXEL_TILT_LEVEL_DATA_HEADER
#else
#include "apps/tilt/tilt_level_data.hpp"
#endif

namespace tilt {

struct ModelOutcome final {
    bool wall_hit{};
    bool bumper_hit{};
    bool star_collected{};
    bool pressure_plate_activated{};
    bool teleported{};
    bool fell{};
    bool completed{};
};

class TiltModel final {
   public:
    void Reset(uint32_t level_index = 0U);
    [[nodiscard]] ModelOutcome Advance(uint64_t delta_us, Vec2 tilt);

    [[nodiscard]] constexpr PointF ball() const { return ball_; }
    [[nodiscard]] constexpr Vec2 velocity() const { return velocity_; }
    [[nodiscard]] constexpr uint32_t level_index() const { return level_index_; }
    [[nodiscard]] constexpr uint32_t collected_stars() const { return collected_stars_; }
    [[nodiscard]] constexpr bool star_collected(uint32_t index) const {
        return index < kStarCount && (collected_star_mask_ & (1U << index)) != 0U;
    }
    [[nodiscard]] constexpr bool goal_unlocked() const { return collected_stars_ >= level().required_stars; }
    [[nodiscard]] constexpr bool completed() const { return completed_; }
    [[nodiscard]] constexpr uint64_t elapsed_us() const { return elapsed_us_; }
    [[nodiscard]] constexpr uint32_t fall_count() const { return fall_count_; }
    [[nodiscard]] constexpr const LevelData& level() const { return kLevels[level_index_]; }
    [[nodiscard]] PointF bumper_position() const;
    [[nodiscard]] WallRect moving_wall_rect() const;
    [[nodiscard]] bool fan_active(uint32_t index) const;
    [[nodiscard]] bool gate_open() const;
    [[nodiscard]] constexpr bool pressure_gate_open() const { return pressure_gate_open_; }
    [[nodiscard]] uint32_t mastery_rating() const;

#ifdef MICROPIXEL_MODEL_TESTING
    void SetBallForTesting(PointF position, Vec2 velocity = {}) {
        ball_ = position;
        velocity_ = velocity;
    }
    void SetElapsedForTesting(uint64_t elapsed_us) { elapsed_us_ = elapsed_us; }
    void SetDynamicPhaseOffsetForTesting(uint64_t offset_us) { dynamic_phase_offset_us_ = offset_us; }
#endif

   private:
    [[nodiscard]] bool ResolveWall(const WallRect& wall, ModelOutcome& outcome);
    [[nodiscard]] bool ResolveBumper(ModelOutcome& outcome);
    void ApplyFans(float step_seconds);
    void UpdatePressureGate(ModelOutcome& outcome);
    void UpdatePortals(ModelOutcome& outcome);
    void ResetBall();

    PointF ball_{};
    Vec2 velocity_{};
    uint64_t elapsed_us_{};
    uint64_t dynamic_phase_offset_us_{};
    uint32_t level_index_{};
    uint32_t collected_star_mask_{};
    uint32_t collected_stars_{};
    uint32_t fall_count_{};
    bool pressure_gate_open_{};
    bool portal_armed_{true};
    bool was_on_ice_{};
    bool was_in_active_fan_[kMaximumFanCount]{};
    bool completed_{};
};

}  // namespace tilt

#endif
