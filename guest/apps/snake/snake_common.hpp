#ifndef MICROPIXEL_SNAKE_COMMON_HPP
#define MICROPIXEL_SNAKE_COMMON_HPP

#include <stdint.h>

#include "sdk/micropixel.hpp"
#include "snake_assets.hpp"

namespace snake {

constexpr uint32_t kColumns = 25U;
constexpr uint32_t kRows = 25U;
constexpr uint32_t kInitialLength = 1U;
constexpr uint32_t kMaxLength = 100U;
constexpr uint32_t kDirectionQueueMax = 3U;
constexpr uint32_t kComboDurationMs = 4000U;
constexpr uint32_t kObstacleForwardSafetyCells = 3U;
constexpr int16_t kObstacleHeadSafetyRadius = 2;
constexpr uint32_t kSpeedFoodLifetimeMs = 5000U;
constexpr uint32_t kPoisonFoodLifetimeMs = 6000U;
constexpr uint32_t kInvincibleDurationMs = 4000U;
constexpr uint32_t kRandomSeed = 0x51a9e21dU;
constexpr uint32_t kParticlePoolSize = 50U;
constexpr uint32_t kTrailPoolSize = 12U;
// A 400 ms trail needs at most four simultaneously visible samples at the
// fastest 128 ms movement period. Keep those slots ahead of decorative detail
// when the Host exposes the 64-command retained-scene budget.
constexpr uint32_t kVisibleTrailSlots = 4U;
constexpr uint32_t kPopupPoolSize = 10U;
constexpr uint32_t kThemeCount = 5U;
// 100 logical body slots + 13 mandatory board rectangles + four motion-trail
// slots. Multi-submit frames keep each transport batch bounded at 128 commands.
constexpr uint32_t kRetainedRectSlots = 117U;
constexpr uint32_t kRetainedTextSlots = 27U;
constexpr uint32_t kFoodFrameCount = 16U;
constexpr uint32_t kBurstFrameCount = 12U;
constexpr uint32_t kSpriteSheetColumns = 4U;
constexpr uint32_t kFoodSpriteCellSize = 43U;
constexpr uint32_t kBurstDisplayPhaseCount = 8U;
constexpr uint64_t kBurstDurationUs = 600000U;
constexpr uint64_t kFoodAnimationDurationUs = 600000U;
constexpr uint64_t kRenderTargetPeriodUs = 16667U;
constexpr int16_t kFoodEdgeMargin = 2;
constexpr int32_t kBoardX = 47;
constexpr int32_t kBoardY = 76;
constexpr int32_t kCellPitch = 25;
constexpr int32_t kScreenWidth = 720;
constexpr int32_t kScreenHeight = 720;
constexpr int32_t kActionButtonWidth = 280;
constexpr int32_t kActionButtonHeight = 96;
constexpr int32_t kActionButtonX = (kScreenWidth - kActionButtonWidth) / 2;
constexpr micropixel::SystemFont kActionButtonFont = micropixel::SystemFont::kLarge;
constexpr int32_t kActionButtonFontHeight = 24;
constexpr int32_t kActionButtonTextOpticalOffsetY = -5;
constexpr uint8_t kOverlayOpacity = 180U;
constexpr micropixel::Rect kStartButtonRect{kActionButtonX, 304, kActionButtonWidth, kActionButtonHeight};
constexpr micropixel::Rect kRestartButtonRect{kActionButtonX, 408, kActionButtonWidth, kActionButtonHeight};
constexpr micropixel::Rect kPauseTouchRect{0, 0, 180, kBoardY};

[[nodiscard]] constexpr int32_t ActionButtonTextY(micropixel::Rect bounds) {
    return bounds.y + (bounds.height - kActionButtonFontHeight) / 2 + kActionButtonTextOpticalOffsetY;
}

static_assert(kBurstFrameCount == snake_assets::burst_atlas_frame_count);
static_assert(snake_assets::burst_atlas_count == 4U);
static_assert(kRetainedRectSlots >= kMaxLength + 13U + kVisibleTrailSlots,
              "full-length Snake and its motion trail must fit the retained frame");

// Touch-first leisure curve. Difficulty increases gently with each level while
// the effect hold below prevents the transition itself from hiding movement or
// replaying stale ticks.
constexpr uint32_t kLevelCount = 10U;
constexpr uint32_t kLevelSpeedsMs[kLevelCount] = {200U, 192U, 184U, 176U, 168U, 160U, 152U, 144U, 136U, 128U};
constexpr uint32_t kLevelThresholds[kLevelCount] = {0U, 100U, 350U, 800U, 1500U, 2500U, 4000U, 6000U, 8500U, 12000U};

enum class Direction : uint8_t { kUp, kRight, kDown, kLeft };
enum class FoodType : uint8_t { kNormal, kGolden, kPoison, kSpeed };
enum class Screen : uint8_t { kMenu, kPlaying, kPaused, kGameOver };

inline bool BlocksMovementDuringEffects(uint64_t shake_remaining_us, uint64_t burst_remaining_us, FoodType burst_type) {
    return shake_remaining_us != 0U || (burst_remaining_us != 0U && burst_type == FoodType::kGolden);
}

inline uint64_t AdvanceHeldMotion(uint64_t accumulated_us, uint64_t delta_us, uint64_t period_us) {
    if (accumulated_us >= period_us) {
        return period_us;
    }
    const uint64_t remaining_us = period_us - accumulated_us;
    return delta_us >= remaining_us ? period_us : accumulated_us + delta_us;
}

struct Cell final {
    int16_t x{};
    int16_t y{};
};

struct Food final {
    Cell cell{};
    FoodType type{FoodType::kNormal};
    uint64_t lifetime_us{};
};

struct MoveOutcome final {
    bool changed{};
    bool ate{};
    bool collision{};
    bool level_up{};
    FoodType food_type{FoodType::kNormal};
    uint32_t points{};
};

struct ScheduledTone final {
    micropixel::Tone tone{};
    uint64_t delay_us{};
    bool active{};
};

struct Rgb final {
    uint8_t red{};
    uint8_t green{};
    uint8_t blue{};
};

struct Theme final {
    Rgb accent{};
    Rgb text{};
    Rgb board{};
    Rgb grid{};
    Rgb border{};
};

constexpr Theme kThemes[] = {
    {{16U, 185U, 129U}, {52U, 211U, 153U}, {10U, 10U, 10U}, {11U, 11U, 11U}, {38U, 38U, 38U}},
    {{139U, 92U, 246U}, {167U, 139U, 250U}, {10U, 10U, 10U}, {11U, 11U, 11U}, {38U, 38U, 38U}},
    {{244U, 63U, 94U}, {251U, 113U, 133U}, {10U, 10U, 10U}, {11U, 11U, 11U}, {38U, 38U, 38U}},
    {{6U, 182U, 212U}, {34U, 211U, 238U}, {10U, 10U, 10U}, {11U, 11U, 11U}, {38U, 38U, 38U}},
    {{245U, 158U, 11U}, {251U, 191U, 36U}, {10U, 10U, 10U}, {11U, 11U, 11U}, {38U, 38U, 38U}},
};

struct Particle final {
    bool active{};
    Cell origin{};
    int16_t dx{};
    int16_t dy{};
    uint32_t age_us{};
    uint32_t duration_us{};
    Rgb color{};
    uint8_t size{};
};

struct Trail final {
    bool active{};
    Cell cell{};
    uint32_t age_us{};
};

struct Popup final {
    bool active{};
    Cell cell{};
    uint32_t points{};
    uint32_t age_us{};
    Rgb color{};
    micropixel::SystemFont font{micropixel::SystemFont::kLarge};
};

using Line = micropixel::FixedString<96U>;

inline bool SameCell(Cell first, Cell second) { return first.x == second.x && first.y == second.y; }

inline bool Opposite(Direction first, Direction second) {
    return (first == Direction::kUp && second == Direction::kDown) ||
           (first == Direction::kDown && second == Direction::kUp) ||
           (first == Direction::kLeft && second == Direction::kRight) ||
           (first == Direction::kRight && second == Direction::kLeft);
}

inline Cell StepCell(Cell cell, Direction direction) {
    switch (direction) {
        case Direction::kUp:
            --cell.y;
            break;
        case Direction::kRight:
            ++cell.x;
            break;
        case Direction::kDown:
            ++cell.y;
            break;
        case Direction::kLeft:
            --cell.x;
            break;
    }
    return cell;
}

inline uint32_t NextRandom(uint32_t& state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

inline FoodType FoodTypeFromPercent(uint32_t percent) {
    if (percent < 10U) {
        return FoodType::kSpeed;
    }
    if (percent < 20U) {
        return FoodType::kPoison;
    }
    if (percent < 35U) {
        return FoodType::kGolden;
    }
    return FoodType::kNormal;
}

inline uint32_t LevelForScore(uint32_t score) {
    uint32_t level = 1U;
    for (uint32_t index = 1U; index < kLevelCount; ++index) {
        if (score >= kLevelThresholds[index]) {
            level = index + 1U;
        }
    }
    return level;
}

inline uint32_t LeisurePeriodMs(uint32_t level) {
    const uint32_t index = level == 0U ? 0U : level - 1U;
    return kLevelSpeedsMs[index < kLevelCount ? index : kLevelCount - 1U];
}

inline uint32_t PoisonResultLength(uint32_t previous_length) {
    return previous_length > 2U ? previous_length - 2U : previous_length;
}

inline micropixel::Color AsColor(Rgb color) { return micropixel::Color::Rgb(color.red, color.green, color.blue); }

inline Rgb MixRgb(Rgb foreground, Rgb background, uint32_t opacity) {
    const micropixel::Color mixed = micropixel::Color::Mix(AsColor(foreground), AsColor(background),
                                                           static_cast<uint8_t>(opacity > 255U ? 255U : opacity));
    return Rgb{mixed.red(), mixed.green(), mixed.blue()};
}

inline const Theme& ThemeForLevel(uint32_t level) { return kThemes[(level - 1U) % kThemeCount]; }

inline uint32_t TakeSlots(uint32_t& available, uint32_t wanted) {
    uint32_t taken = wanted < available ? wanted : available;
    available -= taken;
    return taken;
}

inline int32_t AbsoluteValue(int32_t value) { return value < 0 ? -value : value; }

inline int32_t InterpolateAxis(int32_t previous, int32_t current, uint32_t fraction_q8) {
    return previous + ((current - previous) * static_cast<int32_t>(fraction_q8)) / 256;
}

inline bool ClipRectToScreen(micropixel::Rect input, micropixel::Rect& output) {
    output = input.intersection(micropixel::Rect{0, 0, kScreenWidth, kScreenHeight});
    return !output.empty();
}

}  // namespace snake

#endif
