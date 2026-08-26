#include "apps/snake/snake_model.hpp"

namespace snake {

void SnakeModel::Reset(uint32_t random_seed) {
    random_ = random_seed;
    Reset();
}

void SnakeModel::Reset() {
    length_ = kInitialLength;
    body_[0] = Cell{12, 12};
    direction_ = Direction::kUp;
    queue_count_ = 0U;
    obstacle_count_ = 0U;
    growth_queue_ = 0U;
    score_ = 0U;
    level_ = 1U;
    combo_ = 1U;
    max_combo_ = 1U;
    combo_remaining_us_ = 0U;
    combo_duration_us_ = 0U;
    invincible_remaining_us_ = 0U;
    food_eaten_ = 0U;
    alive_ = true;
    PlaceFood();
}

bool SnakeModel::RequestDirection(Direction requested) {
    if (!alive_ || queue_count_ >= kDirectionQueueMax) {
        return false;
    }
    Direction reference = queue_count_ == 0U ? direction_ : queue_[queue_count_ - 1U];
    if (requested == reference || Opposite(reference, requested)) {
        return false;
    }
    // Reject an immediately lethal side turn before it enters an empty input
    // queue. A rapid follow-up reversal is then compared with the unchanged
    // real direction and is rejected by the ordinary Opposite-direction rule.
    if (queue_count_ == 0U && DirectionWouldCollide(requested)) {
        return false;
    }
    queue_[queue_count_++] = requested;
    return true;
}

bool SnakeModel::AdvanceTime(uint64_t delta_us) {
    bool changed = false;
    if (combo_ > 1U) {
        if (delta_us >= combo_remaining_us_) {
            combo_ = 1U;
            combo_remaining_us_ = 0U;
            combo_duration_us_ = 0U;
            changed = true;
        } else {
            combo_remaining_us_ -= delta_us;
        }
    }
    if (invincible_remaining_us_ != 0U) {
        if (delta_us >= invincible_remaining_us_) {
            invincible_remaining_us_ = 0U;
            changed = true;
        } else {
            invincible_remaining_us_ -= delta_us;
        }
    }
    if (food_.lifetime_us != 0U) {
        if (delta_us >= food_.lifetime_us) {
            PlaceFood();
            changed = true;
        } else {
            food_.lifetime_us -= delta_us;
        }
    }
    return changed;
}

MoveOutcome SnakeModel::Move() {
    MoveOutcome outcome{};
    if (!alive_) {
        outcome.collision = true;
        return outcome;
    }

    // Touch input can enqueue several turns before the next movement tick.
    // Consume stale/unsafe turns, but apply at most one safe perpendicular
    // turn. A rejected turn never causes a collision; continuing straight
    // into an Occupied cell still does.
    while (queue_count_ != 0U) {
        const Direction requested = queue_[0];
        for (uint32_t index = 1U; index < queue_count_; ++index) {
            queue_[index - 1U] = queue_[index];
        }
        --queue_count_;
        if (requested == direction_ || Opposite(direction_, requested) || DirectionWouldCollide(requested)) {
            continue;
        }
        direction_ = requested;
        break;
    }

    Cell head = StepCell(body_[0], direction_);

    bool invincible = invincible_remaining_us_ != 0U;
    if (head.x < 0 || head.y < 0 || head.x >= static_cast<int16_t>(kColumns) || head.y >= static_cast<int16_t>(kRows)) {
        if (!invincible) {
            return Collide();
        }
        head.x = static_cast<int16_t>((head.x + static_cast<int16_t>(kColumns)) % static_cast<int16_t>(kColumns));
        head.y = static_cast<int16_t>((head.y + static_cast<int16_t>(kRows)) % static_cast<int16_t>(kRows));
    }
    if (!invincible) {
        for (uint32_t index = 0U; index < length_; ++index) {
            if (SameCell(head, body_[index])) {
                return Collide();
            }
        }
        for (uint32_t index = 0U; index < obstacle_count_; ++index) {
            if (SameCell(head, obstacles_[index])) {
                return Collide();
            }
        }
    }

    uint32_t previous_length = length_;
    uint32_t shifted_length = length_ < kMaxLength ? length_ + 1U : length_;
    for (uint32_t index = shifted_length - 1U; index != 0U; --index) {
        body_[index] = body_[index - 1U];
    }
    body_[0] = head;
    length_ = shifted_length;
    outcome.changed = true;

    bool ate = SameCell(head, food_.cell);
    if (!ate) {
        if (growth_queue_ != 0U) {
            --growth_queue_;
        } else {
            length_ = previous_length;
        }
        return outcome;
    }

    outcome.ate = true;
    outcome.food_type = food_.type;
    ++food_eaten_;
    uint32_t old_level = level_;
    if (food_.type == FoodType::kPoison) {
        outcome.points = 5U * combo_ * (invincible ? 2U : 1U);
        length_ = PoisonResultLength(previous_length);
    } else if (food_.type == FoodType::kSpeed) {
        invincible_remaining_us_ = static_cast<uint64_t>(kInvincibleDurationMs) * 1000U;
        outcome.points = 20U * combo_ * 2U;
        IncrementCombo();
    } else {
        uint32_t base = food_.type == FoodType::kGolden ? 30U : 10U;
        outcome.points = base * combo_ * (invincible ? 2U : 1U);
        if (food_.type == FoodType::kGolden) {
            ++growth_queue_;
        }
        IncrementCombo();
    }
    score_ += outcome.points;
    level_ = LevelForScore(score_);
    PlaceFood();
    if (level_ > old_level) {
        GenerateObstacles();
        outcome.level_up = true;
    }
    return outcome;
}

uint64_t SnakeModel::LifetimeFor(FoodType type) {
    if (type == FoodType::kSpeed) {
        return static_cast<uint64_t>(kSpeedFoodLifetimeMs) * 1000U;
    }
    return type == FoodType::kPoison ? static_cast<uint64_t>(kPoisonFoodLifetimeMs) * 1000U : 0U;
}

MoveOutcome SnakeModel::Collide() {
    alive_ = false;
    MoveOutcome outcome{};
    outcome.collision = true;
    return outcome;
}

void SnakeModel::IncrementCombo() {
    ++combo_;
    if (combo_ > max_combo_) {
        max_combo_ = combo_;
    }
}

void SnakeModel::ArmComboForCurrentFood() {
    if (combo_ <= 1U || food_.cell.x < 0 || food_.cell.y < 0) {
        return;
    }
    combo_duration_us_ = static_cast<uint64_t>(kComboDurationMs) * 1000U;
    combo_remaining_us_ = combo_duration_us_;
}

bool SnakeModel::DirectionWouldCollide(Direction direction) const {
    if (invincible_remaining_us_ != 0U) {
        return false;
    }
    const Cell candidate = StepCell(body_[0], direction);
    if (candidate.x < 0 || candidate.y < 0 || candidate.x >= static_cast<int16_t>(kColumns) ||
        candidate.y >= static_cast<int16_t>(kRows)) {
        return true;
    }
    return Occupied(candidate, true);
}

bool SnakeModel::Occupied(Cell candidate, bool include_obstacles) const {
    for (uint32_t index = 0U; index < length_; ++index) {
        if (SameCell(candidate, body_[index])) {
            return true;
        }
    }
    if (include_obstacles) {
        for (uint32_t index = 0U; index < obstacle_count_; ++index) {
            if (SameCell(candidate, obstacles_[index])) {
                return true;
            }
        }
    }
    return false;
}

Cell SnakeModel::RandomCell() {
    return Cell{static_cast<int16_t>(NextRandom(random_) % kColumns),
                static_cast<int16_t>(NextRandom(random_) % kRows)};
}

Cell SnakeModel::RandomFoodCell() {
    constexpr uint32_t kInnerColumns = kColumns - 2U * kFoodEdgeMargin;
    constexpr uint32_t kInnerRows = kRows - 2U * kFoodEdgeMargin;
    return Cell{
        static_cast<int16_t>(kFoodEdgeMargin + NextRandom(random_) % kInnerColumns),
        static_cast<int16_t>(kFoodEdgeMargin + NextRandom(random_) % kInnerRows),
    };
}

void SnakeModel::CommitFood(Cell candidate) {
    micropixel::Assert(
        candidate.x >= kFoodEdgeMargin && candidate.x < static_cast<int16_t>(kColumns) - kFoodEdgeMargin &&
            candidate.y >= kFoodEdgeMargin && candidate.y < static_cast<int16_t>(kRows) - kFoodEdgeMargin,
        "snake: food escaped the two-cell safety margin");
    food_.cell = candidate;
    uint32_t percent = static_cast<uint32_t>((static_cast<uint64_t>(NextRandom(random_)) * 100U) >> 32U);
    food_.type = FoodTypeFromPercent(percent);
    food_.lifetime_us = LifetimeFor(food_.type);
    ArmComboForCurrentFood();
}

void SnakeModel::PlaceFood() {
    if (length_ + obstacle_count_ >= kColumns * kRows) {
        food_.cell = Cell{-1, -1};
        food_.lifetime_us = 0U;
        return;
    }
    // Enforce a two-cell safety margin so food never demands a last-second
    // turn against the wall. Random placement is followed by a deterministic
    // scan of the same inner 21x21 region; there is no edge fallback.
    for (uint32_t attempt = 0U; attempt < kColumns * kRows * 2U; ++attempt) {
        Cell candidate = RandomFoodCell();
        if (!Occupied(candidate, true)) {
            CommitFood(candidate);
            return;
        }
    }
    for (uint32_t y = kFoodEdgeMargin; y < kRows - kFoodEdgeMargin; ++y) {
        for (uint32_t x = kFoodEdgeMargin; x < kColumns - kFoodEdgeMargin; ++x) {
            Cell candidate{static_cast<int16_t>(x), static_cast<int16_t>(y)};
            if (!Occupied(candidate, true)) {
                CommitFood(candidate);
                return;
            }
        }
    }
    food_.cell = Cell{-1, -1};
    food_.lifetime_us = 0U;
}

void SnakeModel::GenerateObstacles() {
    obstacle_count_ = 0U;
    if (level_ < 3U) {
        return;
    }
    uint32_t wanted = level_ - 1U < 5U ? level_ - 1U : 5U;
    for (uint32_t attempt = 0U; attempt < kColumns * kRows * 2U && obstacle_count_ < wanted; ++attempt) {
        Cell candidate = RandomCell();
        if (!ObstacleSpawnSafe(candidate) || Occupied(candidate, false)) {
            continue;
        }
        bool duplicate = false;
        for (uint32_t index = 0U; index < obstacle_count_; ++index) {
            if (SameCell(candidate, obstacles_[index])) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            obstacles_[obstacle_count_++] = candidate;
        }
    }
}

bool SnakeModel::ObstacleSpawnSafe(Cell candidate) const {
    if (SameCell(candidate, food_.cell)) {
        return false;
    }
    const Cell head = body_[0];
    const int32_t dx = static_cast<int32_t>(candidate.x) - head.x;
    const int32_t dy = static_cast<int32_t>(candidate.y) - head.y;
    if (dx >= -kObstacleHeadSafetyRadius && dx <= kObstacleHeadSafetyRadius && dy >= -kObstacleHeadSafetyRadius &&
        dy <= kObstacleHeadSafetyRadius) {
        return false;
    }

    // Predict the already committed input queue as well as the current
    // direction. A level-up can never materialize a rock in any of the
    // next three cells the snake is guaranteed to traverse.
    Cell projected = head;
    Direction projected_direction = direction_;
    uint32_t queue_index = 0U;
    for (uint32_t step = 0U; step < kObstacleForwardSafetyCells; ++step) {
        if (queue_index < queue_count_) {
            projected_direction = queue_[queue_index++];
        }
        projected = StepCell(projected, projected_direction);
        projected.x =
            static_cast<int16_t>((projected.x + static_cast<int16_t>(kColumns)) % static_cast<int16_t>(kColumns));
        projected.y = static_cast<int16_t>((projected.y + static_cast<int16_t>(kRows)) % static_cast<int16_t>(kRows));
        if (SameCell(candidate, projected)) {
            return false;
        }
    }
    return true;
}

}  // namespace snake
