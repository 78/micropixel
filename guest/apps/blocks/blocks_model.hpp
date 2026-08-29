#ifndef MICROPIXEL_BLOCKS_MODEL_HPP
#define MICROPIXEL_BLOCKS_MODEL_HPP

#include "apps/blocks/blocks_common.hpp"

namespace blocks {

class BlocksModel final {
   public:
    void Reset(uint32_t random_seed = kDefaultRandomSeed);

    [[nodiscard]] bool MoveHorizontal(int32_t direction);
    [[nodiscard]] bool RotateClockwise();
    [[nodiscard]] LockOutcome SoftDrop();
    [[nodiscard]] LockOutcome Tick();
    [[nodiscard]] LockOutcome HardDrop();
    [[nodiscard]] bool Hold();

    [[nodiscard]] static bool ShapeOccupied(Tetromino type, uint32_t rotation, uint32_t x, uint32_t y);
    [[nodiscard]] uint8_t board_cell(uint32_t x, uint32_t y) const { return board_[y][x]; }
    [[nodiscard]] const ActivePiece& active() const { return active_; }
    [[nodiscard]] Tetromino next() const { return next_; }
    [[nodiscard]] Tetromino held() const { return held_; }
    [[nodiscard]] bool has_hold() const { return has_hold_; }
    [[nodiscard]] bool hold_available() const { return !hold_used_; }
    [[nodiscard]] bool alive() const { return alive_; }
    [[nodiscard]] uint32_t score() const { return score_; }
    [[nodiscard]] uint32_t lines() const { return lines_; }
    [[nodiscard]] uint32_t level() const { return level_; }
    [[nodiscard]] uint32_t combo() const { return combo_; }
    [[nodiscard]] int32_t ghost_y() const;
    [[nodiscard]] uint32_t drop_period_us() const;

#if defined(MICROPIXEL_MODEL_TESTING)
    void SetCellForTesting(uint32_t x, uint32_t y, uint8_t value) { board_[y][x] = value; }
    void SetActiveForTesting(ActivePiece piece) { active_ = piece; }
    void SetLevelForTesting(uint32_t level) { level_ = level; }
#endif

   private:
    [[nodiscard]] uint32_t NextRandom();
    void RefillBag();
    [[nodiscard]] Tetromino TakeFromBag();
    void SpawnNext();
    void SpawnType(Tetromino type);
    [[nodiscard]] bool Fits(const ActivePiece& piece) const;
    [[nodiscard]] LockOutcome StepDown(bool award_soft_drop);
    [[nodiscard]] LockOutcome LockActive();
    [[nodiscard]] uint32_t ClearCompletedRows(uint32_t& cleared_rows_mask);

    uint8_t board_[kBoardRows][kBoardColumns]{};
    Tetromino bag_[kTetrominoCount]{};
    ActivePiece active_{};
    Tetromino next_{Tetromino::kI};
    Tetromino held_{Tetromino::kI};
    uint32_t random_{kDefaultRandomSeed};
    uint32_t bag_index_{kTetrominoCount};
    uint32_t score_{};
    uint32_t lines_{};
    uint32_t level_{1U};
    uint32_t combo_{};
    bool has_hold_{};
    bool hold_used_{};
    bool alive_{};
};

}  // namespace blocks

#endif
