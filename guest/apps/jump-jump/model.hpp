// SPDX-License-Identifier: Apache-2.0
#ifndef MICROPIXEL_JUMP_JUMP_MODEL_HPP
#define MICROPIXEL_JUMP_JUMP_MODEL_HPP

#include <stdint.h>

namespace jump_jump {

struct Vec2 final {
    float x{};
    float z{};
    constexpr Vec2 operator+(Vec2 b) const { return {x + b.x, z + b.z}; }
    constexpr Vec2 operator-(Vec2 b) const { return {x - b.x, z - b.z}; }
    constexpr Vec2 operator*(float s) const { return {x * s, z * s}; }
};

float Length(Vec2 v);
float Clamp(float value, float low, float high);
float Ease(float t);

enum class PlatformKind : uint8_t { kBox, kCylinder, kDrain, kCube, kShop, kRecord, kStool };
enum class Phase : uint8_t { kReady, kCharging, kFlying, kSettling, kFalling, kGameOver };
enum class Landing : uint8_t { kMiss, kEdge, kSafe, kCenter };
enum Feedback : uint32_t {
    kNoFeedback = 0U,
    kJump = 1U,
    kLand = 2U,
    kPerfect = 4U,
    kBonus = 8U,
    kFail = 16U,
};

struct Platform final {
    Vec2 position{};
    float size{78.0F};
    PlatformKind kind{PlatformKind::kBox};
    uint8_t color{};
    bool rewarded{};
};

struct Pose final {
    Vec2 position{};
    float height{};
    float squash{};
    float rotation{};
};

class Model final {
   public:
    static constexpr uint64_t kFlightUs = 480000U;
    static constexpr uint64_t kSettleUs = 220000U;
    static constexpr uint64_t kFallUs = 650000U;
    static constexpr uint64_t kBonusUs = 2000000U;
    static constexpr uint64_t kMaximumChargeUs = 1200000U;
    static constexpr float kPlatformHeight = 46.0F;

    void Reset(uint32_t seed, uint64_t now_us);
    void Restart(uint64_t now_us);
    bool Press(uint64_t now_us);
    bool Release(uint64_t now_us);
    void Cancel(uint64_t now_us);
    void Advance(uint64_t now_us);
    [[nodiscard]] Pose pose() const;
    [[nodiscard]] Vec2 camera() const;
    [[nodiscard]] float charge() const;
    [[nodiscard]] uint64_t phase_age_us() const;
    [[nodiscard]] uint64_t reward_age_us() const;
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] const Platform& current() const { return current_; }
    [[nodiscard]] const Platform& next() const { return next_; }
    [[nodiscard]] const Platform& previous() const { return previous_; }
    [[nodiscard]] bool has_previous() const { return has_previous_; }
    [[nodiscard]] uint32_t score() const { return score_; }
    [[nodiscard]] uint32_t combo() const { return combo_; }
    [[nodiscard]] uint32_t jumps() const { return jumps_; }
    [[nodiscard]] uint32_t last_award() const { return last_award_; }
    [[nodiscard]] Landing last_landing() const { return last_landing_; }
    [[nodiscard]] Vec2 reward_position() const { return reward_position_; }
    [[nodiscard]] uint32_t TakeFeedback();
    [[nodiscard]] static float Distance(uint64_t hold_us);
    [[nodiscard]] static Landing Classify(const Platform& platform, Vec2 position);
    [[nodiscard]] static uint32_t Bonus(PlatformKind kind);

   private:
    uint32_t Random();
    void GenerateNext();
    void Land(uint64_t at_us);
    void Award(uint32_t amount, uint64_t at_us);

    Platform previous_{};
    Platform current_{};
    Platform next_{};
    Vec2 player_{};
    Vec2 launch_{};
    Vec2 destination_{};
    Vec2 camera_from_{};
    Vec2 camera_to_{};
    Vec2 reward_position_{};
    Phase phase_{Phase::kReady};
    Landing last_landing_{Landing::kSafe};
    uint64_t now_us_{};
    uint64_t phase_at_us_{};
    uint64_t camera_at_us_{};
    uint64_t idle_at_us_{};
    uint64_t reward_at_us_{};
    uint32_t rng_{1U};
    uint32_t score_{};
    uint32_t combo_{};
    uint32_t jumps_{};
    uint32_t last_award_{};
    uint32_t feedback_{};
    uint32_t last_special_jump_{};
    uint32_t last_special_slot_{UINT32_MAX};
    uint32_t size_steps_{};
    bool has_previous_{};
};

// A single owner across touch and key input. A second finger/key cannot
// replace a held press or release it. Resume/Cancel never launch a jump.
class Input final {
   public:
    bool Down(Model& model, uint64_t token, uint64_t at_us);
    bool Up(Model& model, uint64_t token, uint64_t at_us);
    void Cancel(Model& model, uint64_t token, uint64_t at_us);
    void Reset(Model& model, uint64_t at_us);
    [[nodiscard]] bool active() const { return active_; }

   private:
    uint64_t token_{};
    bool active_{};
    bool restart_press_{};
};

}  // namespace jump_jump
#endif
