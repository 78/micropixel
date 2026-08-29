#include "apps/blocks/blocks_model.hpp"

namespace blocks {

namespace {

constexpr uint16_t kShapeMasks[kTetrominoCount][4U] = {
    {0x00f0U, 0x4444U, 0x0f00U, 0x2222U},  // I
    {0x0071U, 0x0226U, 0x0470U, 0x0322U},  // J
    {0x0074U, 0x0622U, 0x0170U, 0x0223U},  // L
    {0x0066U, 0x0066U, 0x0066U, 0x0066U},  // O
    {0x0036U, 0x0462U, 0x0360U, 0x0231U},  // S
    {0x0072U, 0x0262U, 0x0270U, 0x0232U},  // T
    {0x0063U, 0x0264U, 0x0630U, 0x0132U},  // Z
};

constexpr int8_t kKickOffsets[] = {0, -1, 1, -2, 2};

// P(level) = 750 ms / fourth_root(1 + 8.58 * (level - 1)).
// The curve passes approximately through (12, 240 ms) and (24, 200 ms), then
// keeps accelerating forever while the change between adjacent levels softens.
constexpr float kInitialDropPeriodUs = 750000.0F;
constexpr float kDifficultyGrowthPerLevel = 8.58F;

}  // namespace

void BlocksModel::Reset(uint32_t random_seed) {
    for (uint32_t y = 0U; y < kBoardRows; ++y) {
        for (uint32_t x = 0U; x < kBoardColumns; ++x) {
            board_[y][x] = 0U;
        }
    }
    random_ = random_seed == 0U ? kDefaultRandomSeed : random_seed;
    bag_index_ = kTetrominoCount;
    score_ = 0U;
    lines_ = 0U;
    level_ = 1U;
    combo_ = 0U;
    has_hold_ = false;
    hold_used_ = false;
    alive_ = true;
    next_ = TakeFromBag();
    SpawnNext();
}

bool BlocksModel::ShapeOccupied(Tetromino type, uint32_t rotation, uint32_t x, uint32_t y) {
    if (x >= 4U || y >= 4U) {
        return false;
    }
    const uint32_t type_index = static_cast<uint32_t>(type);
    const uint16_t mask = kShapeMasks[type_index][rotation % 4U];
    return (mask & (1U << (y * 4U + x))) != 0U;
}

uint32_t BlocksModel::NextRandom() {
    random_ = random_ * 1664525U + 1013904223U;
    return random_;
}

void BlocksModel::RefillBag() {
    for (uint32_t index = 0U; index < kTetrominoCount; ++index) {
        bag_[index] = static_cast<Tetromino>(index);
    }
    for (uint32_t index = kTetrominoCount - 1U; index > 0U; --index) {
        const uint32_t other = NextRandom() % (index + 1U);
        const Tetromino temporary = bag_[index];
        bag_[index] = bag_[other];
        bag_[other] = temporary;
    }
    bag_index_ = 0U;
}

Tetromino BlocksModel::TakeFromBag() {
    if (bag_index_ >= kTetrominoCount) {
        RefillBag();
    }
    return bag_[bag_index_++];
}

void BlocksModel::SpawnType(Tetromino type) {
    active_ = ActivePiece{type, 0U, 3, -1};
    if (!Fits(active_)) {
        alive_ = false;
    }
}

void BlocksModel::SpawnNext() {
    const Tetromino type = next_;
    next_ = TakeFromBag();
    hold_used_ = false;
    SpawnType(type);
}

bool BlocksModel::Fits(const ActivePiece& piece) const {
    for (uint32_t local_y = 0U; local_y < 4U; ++local_y) {
        for (uint32_t local_x = 0U; local_x < 4U; ++local_x) {
            if (!ShapeOccupied(piece.type, piece.rotation, local_x, local_y)) {
                continue;
            }
            const int32_t x = static_cast<int32_t>(piece.x) + static_cast<int32_t>(local_x);
            const int32_t y = static_cast<int32_t>(piece.y) + static_cast<int32_t>(local_y);
            if (x < 0 || x >= static_cast<int32_t>(kBoardColumns) || y >= static_cast<int32_t>(kBoardRows)) {
                return false;
            }
            if (y >= 0 && board_[y][x] != 0U) {
                return false;
            }
        }
    }
    return true;
}

bool BlocksModel::MoveHorizontal(int32_t direction) {
    if (!alive_ || (direction != -1 && direction != 1)) {
        return false;
    }
    ActivePiece moved = active_;
    moved.x = static_cast<int8_t>(static_cast<int32_t>(moved.x) + direction);
    if (!Fits(moved)) {
        return false;
    }
    active_ = moved;
    return true;
}

bool BlocksModel::RotateClockwise() {
    if (!alive_ || active_.type == Tetromino::kO) {
        return alive_;
    }
    ActivePiece rotated = active_;
    rotated.rotation = static_cast<uint8_t>((rotated.rotation + 1U) % 4U);
    for (int8_t offset : kKickOffsets) {
        rotated.x = static_cast<int8_t>(static_cast<int32_t>(active_.x) + offset);
        if (Fits(rotated)) {
            active_ = rotated;
            return true;
        }
    }
    rotated.x = active_.x;
    rotated.y = static_cast<int8_t>(static_cast<int32_t>(active_.y) - 1);
    if (Fits(rotated)) {
        active_ = rotated;
        return true;
    }
    return false;
}

LockOutcome BlocksModel::StepDown(bool award_soft_drop) {
    if (!alive_) {
        return LockOutcome{.game_over = true};
    }
    ActivePiece moved = active_;
    ++moved.y;
    if (Fits(moved)) {
        active_ = moved;
        const uint32_t points = award_soft_drop ? 1U : 0U;
        score_ += points;
        return LockOutcome{.points_gained = points, .moved = true};
    }
    return LockActive();
}

LockOutcome BlocksModel::SoftDrop() { return StepDown(true); }

LockOutcome BlocksModel::Tick() { return StepDown(false); }

LockOutcome BlocksModel::HardDrop() {
    if (!alive_) {
        return LockOutcome{.game_over = true};
    }
    uint8_t distance = 0U;
    for (;;) {
        ActivePiece moved = active_;
        ++moved.y;
        if (!Fits(moved)) {
            break;
        }
        active_ = moved;
        ++distance;
    }
    const uint32_t drop_points = static_cast<uint32_t>(distance) * 2U;
    score_ += drop_points;
    LockOutcome outcome = LockActive();
    outcome.drop_distance = distance;
    outcome.points_gained += drop_points;
    return outcome;
}

bool BlocksModel::Hold() {
    if (!alive_ || hold_used_) {
        return false;
    }
    const Tetromino outgoing = active_.type;
    if (has_hold_) {
        const Tetromino incoming = held_;
        held_ = outgoing;
        SpawnType(incoming);
    } else {
        held_ = outgoing;
        has_hold_ = true;
        SpawnNext();
    }
    hold_used_ = true;
    return alive_;
}

uint32_t BlocksModel::ClearCompletedRows(uint32_t& cleared_rows_mask) {
    bool completed[kBoardRows]{};
    uint32_t count = 0U;
    for (uint32_t y = 0U; y < kBoardRows; ++y) {
        bool full = true;
        for (uint32_t x = 0U; x < kBoardColumns; ++x) {
            full = full && board_[y][x] != 0U;
        }
        completed[y] = full;
        if (full) {
            cleared_rows_mask |= 1U << y;
            ++count;
        }
    }

    int32_t write_y = static_cast<int32_t>(kBoardRows) - 1;
    for (int32_t read_y = static_cast<int32_t>(kBoardRows) - 1; read_y >= 0; --read_y) {
        if (completed[read_y]) {
            continue;
        }
        if (write_y != read_y) {
            for (uint32_t x = 0U; x < kBoardColumns; ++x) {
                board_[write_y][x] = board_[read_y][x];
            }
        }
        --write_y;
    }
    while (write_y >= 0) {
        for (uint32_t x = 0U; x < kBoardColumns; ++x) {
            board_[write_y][x] = 0U;
        }
        --write_y;
    }
    return count;
}

LockOutcome BlocksModel::LockActive() {
    LockOutcome outcome{.locked = true};
    for (uint32_t local_y = 0U; local_y < 4U; ++local_y) {
        for (uint32_t local_x = 0U; local_x < 4U; ++local_x) {
            if (!ShapeOccupied(active_.type, active_.rotation, local_x, local_y)) {
                continue;
            }
            const int32_t x = static_cast<int32_t>(active_.x) + static_cast<int32_t>(local_x);
            const int32_t y = static_cast<int32_t>(active_.y) + static_cast<int32_t>(local_y);
            if (y < 0) {
                alive_ = false;
                outcome.game_over = true;
                return outcome;
            }
            board_[y][x] = static_cast<uint8_t>(static_cast<uint32_t>(active_.type) + 1U);
        }
    }

    const uint32_t previous_level = level_;
    const uint32_t cleared = ClearCompletedRows(outcome.cleared_rows_mask);
    outcome.cleared_lines = static_cast<uint8_t>(cleared);
    if (cleared != 0U) {
        ++combo_;
        constexpr uint32_t kLinePoints[] = {0U, 100U, 300U, 500U, 800U};
        const uint32_t combo_bonus = combo_ > 1U ? (combo_ - 1U) * 50U * level_ : 0U;
        const uint32_t gained = kLinePoints[cleared] * level_ + combo_bonus;
        score_ += gained;
        outcome.points_gained += gained;
        lines_ += cleared;
        level_ = lines_ / 10U + 1U;
        outcome.level_up = level_ != previous_level;
    } else {
        combo_ = 0U;
    }

    SpawnNext();
    outcome.game_over = !alive_;
    return outcome;
}

int32_t BlocksModel::ghost_y() const {
    if (!alive_) {
        return active_.y;
    }
    ActivePiece ghost = active_;
    for (;;) {
        ActivePiece moved = ghost;
        ++moved.y;
        if (!Fits(moved)) {
            return ghost.y;
        }
        ghost = moved;
    }
}

uint32_t BlocksModel::drop_period_us() const {
    const uint32_t level_index = level_ > 1U ? level_ - 1U : 0U;
    const float curve_input = 1.0F + kDifficultyGrowthPerLevel * static_cast<float>(level_index);
    const float fourth_root = __builtin_sqrtf(__builtin_sqrtf(curve_input));
    return static_cast<uint32_t>(kInitialDropPeriodUs / fourth_root + 0.5F);
}

}  // namespace blocks
