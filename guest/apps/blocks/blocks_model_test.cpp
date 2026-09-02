#include <assert.h>
#include <stdint.h>

#include "apps/blocks/blocks_model.hpp"

namespace {

void TestEveryRotationContainsFourCells() {
    for (uint32_t type = 0U; type < blocks::kTetrominoCount; ++type) {
        for (uint32_t rotation = 0U; rotation < 4U; ++rotation) {
            uint32_t count = 0U;
            for (uint32_t y = 0U; y < 4U; ++y) {
                for (uint32_t x = 0U; x < 4U; ++x) {
                    count += blocks::BlocksModel::ShapeOccupied(static_cast<blocks::Tetromino>(type), rotation, x, y)
                                 ? 1U
                                 : 0U;
                }
            }
            assert(count == 4U);
        }
    }
}

void TestGestureDirectionLockClassification() {
    using blocks::GestureAxis;

    assert(blocks::ClassifyGestureAxis(23, 0) == GestureAxis::kUndecided);
    assert(blocks::ClassifyGestureAxis(0, 23) == GestureAxis::kUndecided);
    assert(blocks::ClassifyGestureAxis(30, 30) == GestureAxis::kUndecided);
    assert(blocks::ClassifyGestureAxis(31, 90) == GestureAxis::kVertical);
    assert(blocks::ClassifyGestureAxis(-35, 120) == GestureAxis::kVertical);
    assert(blocks::ClassifyGestureAxis(90, 25) == GestureAxis::kHorizontal);
    assert(blocks::ClassifyGestureAxis(-90, 25) == GestureAxis::kHorizontal);
}

void TestHorizontalBoundsAndHardDrop() {
    blocks::BlocksModel model;
    model.Reset(1U);
    while (model.MoveHorizontal(-1)) {
    }
    assert(!model.MoveHorizontal(-1));
    const blocks::LockOutcome outcome = model.HardDrop();
    assert(outcome.locked);
    assert(outcome.drop_distance > 0U);
    assert(model.score() == static_cast<uint32_t>(outcome.drop_distance) * 2U);

    uint32_t occupied = 0U;
    for (uint32_t y = 0U; y < blocks::kBoardRows; ++y) {
        for (uint32_t x = 0U; x < blocks::kBoardColumns; ++x) {
            occupied += model.board_cell(x, y) != 0U ? 1U : 0U;
        }
    }
    assert(occupied == 4U);
}

void TestSingleLineClearAndScoring() {
    blocks::BlocksModel model;
    model.Reset(2U);
    for (uint32_t x = 0U; x < 6U; ++x) {
        model.SetCellForTesting(x, blocks::kBoardRows - 1U, 1U);
    }
    model.SetActiveForTesting(blocks::ActivePiece{blocks::Tetromino::kI, 0U, 6, 18});
    const blocks::LockOutcome outcome = model.HardDrop();
    assert(outcome.locked);
    assert(outcome.cleared_lines == 1U);
    assert(outcome.cleared_rows_mask == (1U << (blocks::kBoardRows - 1U)));
    assert(model.lines() == 1U);
    assert(model.score() == 100U);
    for (uint32_t x = 0U; x < blocks::kBoardColumns; ++x) {
        assert(model.board_cell(x, blocks::kBoardRows - 1U) == 0U);
    }
}

void TestHoldIsLimitedUntilLock() {
    blocks::BlocksModel model;
    model.Reset(3U);
    const blocks::Tetromino original = model.active().type;
    assert(model.Hold());
    assert(model.has_hold());
    assert(model.held() == original);
    assert(!model.Hold());
    (void)model.HardDrop();
    assert(model.hold_available());
}

void TestCappedDifficultyCurve() {
    blocks::BlocksModel model;
    model.Reset(4U);

    struct ExpectedPeriod final {
        uint32_t level;
        uint32_t period_us;
    };
    constexpr ExpectedPeriod kExpected[] = {
        {1U, 750000U},
        {12U, 240000U},
        {20U, 200000U},
        {99U, 100000U},
        {100U, 100000U},
        {200U, 100000U},
    };
    for (const ExpectedPeriod expected : kExpected) {
        model.SetLevelForTesting(expected.level);
        assert(model.drop_period_us() == expected.period_us);
    }

    uint32_t previous_period_us = 750000U;
    for (uint32_t level = 1U; level <= 99U; ++level) {
        model.SetLevelForTesting(level);
        const uint32_t period_us = model.drop_period_us();
        assert(period_us < previous_period_us || level == 1U);
        previous_period_us = period_us;
    }
}

uint32_t NextRandom(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

void TestRandomPlayAndRenderRunBound() {
    uint32_t random = 0x75a1b2c3U;
    for (uint32_t game = 0U; game < 200U; ++game) {
        blocks::BlocksModel model;
        model.Reset(NextRandom(random));
        uint32_t previous_score = 0U;
        for (uint32_t action = 0U; action < 1200U && model.alive(); ++action) {
            switch (NextRandom(random) % 8U) {
                case 0U:
                    (void)model.MoveHorizontal(-1);
                    break;
                case 1U:
                    (void)model.MoveHorizontal(1);
                    break;
                case 2U:
                    (void)model.RotateClockwise();
                    break;
                case 3U:
                    (void)model.Hold();
                    break;
                case 4U:
                    (void)model.SoftDrop();
                    break;
                case 5U:
                    (void)model.Tick();
                    break;
                default:
                    (void)model.HardDrop();
                    break;
            }
            assert(model.score() >= previous_score);
            previous_score = model.score();
            const uint32_t expected_level =
                model.lines() / 10U >= 98U ? 99U : model.lines() / 10U + 1U;
            assert(model.level() == expected_level);
            if (model.alive()) {
                assert(model.ghost_y() >= model.active().y);
            }
            uint32_t occupancy_runs = 0U;
            for (uint32_t y = 0U; y < blocks::kBoardRows; ++y) {
                bool inside_run = false;
                for (uint32_t x = 0U; x < blocks::kBoardColumns; ++x) {
                    const uint8_t value = model.board_cell(x, y);
                    assert(value <= blocks::kTetrominoCount);
                    if (value != 0U && !inside_run) {
                        ++occupancy_runs;
                    }
                    inside_run = value != 0U;
                }
            }
            assert(occupancy_runs <= 100U);
        }
    }
}

}  // namespace

int main() {
    TestEveryRotationContainsFourCells();
    TestGestureDirectionLockClassification();
    TestHorizontalBoundsAndHardDrop();
    TestSingleLineClearAndScoring();
    TestHoldIsLimitedUntilLock();
    TestCappedDifficultyCurve();
    TestRandomPlayAndRenderRunBound();
    return 0;
}
