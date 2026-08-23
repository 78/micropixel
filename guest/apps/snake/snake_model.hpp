#ifndef MICROPIXEL_SNAKE_MODEL_HPP
#define MICROPIXEL_SNAKE_MODEL_HPP

#include "apps/snake/snake_common.hpp"

namespace snake {

class SnakeModel final {
   public:
    void Reset();

    void Reset(uint32_t random_seed);

    bool RequestDirection(Direction requested);

    bool AdvanceTime(uint64_t delta_us);

    MoveOutcome Move();

    [[nodiscard]] const Cell* body() const { return body_; }
    [[nodiscard]] uint32_t length() const { return length_; }
    [[nodiscard]] const Food& food() const { return food_; }
    [[nodiscard]] const Cell* obstacles() const { return obstacles_; }
    [[nodiscard]] uint32_t obstacle_count() const { return obstacle_count_; }
    [[nodiscard]] uint32_t score() const { return score_; }
    [[nodiscard]] uint32_t level() const { return level_; }
    [[nodiscard]] uint32_t combo() const { return combo_; }
    [[nodiscard]] uint64_t combo_remaining_us() const { return combo_remaining_us_; }
    [[nodiscard]] uint64_t combo_duration_us() const { return combo_duration_us_; }
    [[nodiscard]] uint32_t max_combo() const { return max_combo_; }
    [[nodiscard]] uint32_t food_eaten() const { return food_eaten_; }
    [[nodiscard]] bool alive() const { return alive_; }
    [[nodiscard]] bool invincible() const { return invincible_remaining_us_ != 0U; }
    [[nodiscard]] uint64_t invincible_remaining_us() const { return invincible_remaining_us_; }
    [[nodiscard]] Direction direction() const { return direction_; }
    [[nodiscard]] uint32_t queue_count() const { return queue_count_; }

   private:
    static uint64_t LifetimeFor(FoodType type);

    MoveOutcome Collide();

    void IncrementCombo();

    void ArmComboForCurrentFood();

    bool DirectionWouldCollide(Direction direction) const;

    bool Occupied(Cell candidate, bool include_obstacles) const;

    Cell RandomCell();

    Cell RandomFoodCell();

    void CommitFood(Cell candidate);

    void PlaceFood();

    void GenerateObstacles();

    bool ObstacleSpawnSafe(Cell candidate) const;

    Cell body_[kMaxLength]{};
    Cell obstacles_[5]{};
    Direction queue_[kDirectionQueueMax]{};
    uint32_t length_{};
    Food food_{};
    Direction direction_{Direction::kUp};
    uint32_t queue_count_{};
    uint32_t obstacle_count_{};
    uint32_t growth_queue_{};
    uint32_t score_{};
    uint32_t level_{1U};
    uint32_t combo_{1U};
    uint32_t max_combo_{1U};
    uint64_t combo_remaining_us_{};
    uint64_t combo_duration_us_{};
    uint64_t invincible_remaining_us_{};
    uint32_t food_eaten_{};
    uint32_t random_{kRandomSeed};
    bool alive_{};
};

}  // namespace snake

#endif
