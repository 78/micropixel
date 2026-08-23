#ifndef MICROPIXEL_BLOCKS_COMMON_HPP
#define MICROPIXEL_BLOCKS_COMMON_HPP

#include <stdint.h>

#include "sdk/micropixel.hpp"

namespace blocks {

constexpr uint32_t kBoardColumns = 10U;
constexpr uint32_t kBoardRows = 20U;
constexpr uint32_t kTetrominoCount = 7U;
constexpr uint32_t kThemeCount = 5U;
constexpr uint32_t kScreenWidth = 720U;
constexpr uint32_t kScreenHeight = 720U;
constexpr int32_t kBoardX = 98;
constexpr int32_t kBoardY = 76;
constexpr int32_t kBoardAssetWidth = 524;
constexpr int32_t kBoardAssetHeight = 600;
constexpr int32_t kCellPitch = 30;
constexpr int32_t kPlayfieldWidth = static_cast<int32_t>(kBoardColumns) * kCellPitch;
constexpr int32_t kPlayfieldHeight = static_cast<int32_t>(kBoardRows) * kCellPitch;
constexpr int32_t kSidebarX = 328;
constexpr int32_t kSidebarWidth = kBoardAssetWidth - kSidebarX;
constexpr int32_t kPlayfieldCenterX = kBoardX + kPlayfieldWidth / 2;
constexpr int32_t kScreenCenterX = static_cast<int32_t>(kScreenWidth) / 2;
constexpr uint64_t kRenderTargetPeriodUs = 16667U;
constexpr uint32_t kDefaultRandomSeed = 0x4b10c5e7U;

constexpr int32_t kActionButtonWidth = 280;
constexpr int32_t kActionButtonHeight = 96;
constexpr int32_t kActionButtonX = kScreenCenterX - kActionButtonWidth / 2;
constexpr uint16_t kActionButtonFontSize = 24U;
constexpr int32_t kActionButtonTextOpticalOffsetY = -2;
constexpr uint8_t kOverlayOpacity = 216U;
constexpr micropixel::Rect kStartButtonRect{kActionButtonX, 304, kActionButtonWidth, kActionButtonHeight};
constexpr micropixel::Rect kRestartButtonRect{kActionButtonX, 408, kActionButtonWidth, kActionButtonHeight};
constexpr micropixel::Rect kGameOverOverlayRect{60, kBoardY, 600, kBoardAssetHeight};
constexpr micropixel::Rect kPauseTouchRect{0, 0, 180, kBoardY};
constexpr micropixel::Rect kHoldTouchRect{kBoardX + kSidebarX, kBoardY, kSidebarWidth, 126};
constexpr micropixel::Rect kPlayTouchRect{0, 0, static_cast<int32_t>(kScreenWidth),
                                          static_cast<int32_t>(kScreenHeight)};

[[nodiscard]] constexpr int32_t ActionButtonTextY(micropixel::Rect bounds) {
    return bounds.y + (bounds.height - static_cast<int32_t>(kActionButtonFontSize)) / 2 +
           kActionButtonTextOpticalOffsetY;
}

enum class Tetromino : uint8_t { kI, kJ, kL, kO, kS, kT, kZ };
enum class Screen : uint8_t { kMenu, kPlaying, kPaused, kGameOver };

struct Cell final {
    int8_t x{};
    int8_t y{};
};

struct ActivePiece final {
    Tetromino type{Tetromino::kI};
    uint8_t rotation{};
    int8_t x{};
    int8_t y{};
};

struct LockOutcome final {
    uint32_t points_gained{};
    uint32_t cleared_rows_mask{};
    uint8_t cleared_lines{};
    uint8_t drop_distance{};
    bool moved{};
    bool locked{};
    bool level_up{};
    bool game_over{};
};

struct Rgb final {
    uint8_t red{};
    uint8_t green{};
    uint8_t blue{};
};

struct Theme final {
    Rgb accent{};
    Rgb text{};
};

constexpr Theme kThemes[] = {
    {{16U, 185U, 129U}, {52U, 211U, 153U}}, {{139U, 92U, 246U}, {167U, 139U, 250U}},
    {{244U, 63U, 94U}, {251U, 113U, 133U}}, {{6U, 182U, 212U}, {34U, 211U, 238U}},
    {{245U, 158U, 11U}, {251U, 191U, 36U}},
};

constexpr Rgb kTetrominoColors[] = {
    {34U, 211U, 238U},   // I / cyan
    {59U, 130U, 246U},   // J / blue
    {245U, 158U, 11U},   // L / amber
    {251U, 191U, 36U},   // O / yellow
    {52U, 211U, 153U},   // S / mint
    {167U, 139U, 250U},  // T / violet
    {251U, 113U, 133U},  // Z / rose
};

using Line = micropixel::FixedString<96U>;

[[nodiscard]] inline micropixel::Color AsColor(Rgb color) {
    return micropixel::Color::Rgb(color.red, color.green, color.blue);
}

[[nodiscard]] inline const Theme& ThemeForLevel(uint32_t level) {
    return kThemes[(level == 0U ? 0U : level - 1U) % kThemeCount];
}

[[nodiscard]] inline Rgb ColorForTetromino(Tetromino type) { return kTetrominoColors[static_cast<uint32_t>(type)]; }

[[nodiscard]] inline int32_t AbsoluteValue(int32_t value) { return value < 0 ? -value : value; }

}  // namespace blocks

#endif
