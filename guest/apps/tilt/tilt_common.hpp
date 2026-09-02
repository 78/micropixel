#ifndef MICROPIXEL_TILT_COMMON_HPP
#define MICROPIXEL_TILT_COMMON_HPP

#include <stdint.h>

#include "sdk/micropixel.hpp"

namespace tilt {

constexpr uint32_t kScreenWidth = 720U;
constexpr uint32_t kScreenHeight = 720U;
constexpr int32_t kLevelSize = 600;
constexpr int32_t kBoardX = 60;
constexpr int32_t kBoardY = 96;
constexpr int32_t kBoardSize = 600;
constexpr int32_t kHudHeight = 84;
constexpr float kBallRadius = 18.0F;
constexpr uint64_t kRenderTargetPeriodUs = 16667U;
constexpr uint32_t kObjectFrameLogicalPixels = 96U;
constexpr uint32_t kObjectSheetColumns = 4U;
constexpr uint32_t kObjectFrameCount = 16U;
constexpr uint32_t kFanFrameCount = 8U;
constexpr uint32_t kMechanicSheetColumns = 4U;
constexpr uint32_t kMechanicFrameCount = 8U;
constexpr uint32_t kStarCount = 3U;
constexpr uint32_t kMaximumFanCount = 2U;
constexpr uint32_t kMaximumWallCount = 160U;
constexpr uint32_t kMaximumWallJointCount = 64U;
constexpr uint32_t kMaximumVisualWallBlockCount = 172U;
constexpr uint32_t kBoardTileSheetColumns = 5U;
constexpr uint32_t kBoardTileFrameCount = 10U;
constexpr uint32_t kTrailCapacity = 10U;
constexpr uint32_t kParticleCapacity = 16U;

constexpr micropixel::Rect kBoardRect{kBoardX, kBoardY, kBoardSize, kBoardSize};
constexpr micropixel::Rect kActionButtonRect{210, 390, 300, 96};
// The 320x240 profile maps its 12 px medium font into the 720-unit logical
// viewport at 3x. "RUN FROM 01" measures 255 logical units there, so keep the
// secondary action as wide as the primary action instead of the old 240-unit
// button.
constexpr micropixel::Rect kSecondaryButtonRect{210, 510, 300, 72};
constexpr micropixel::Rect kPreviousLevelButtonRect{135, 270, 100, 80};
constexpr micropixel::Rect kNextLevelButtonRect{485, 270, 100, 80};
constexpr micropixel::Rect kPauseTouchRect{12, 4, 272, 78};

enum class Screen : uint8_t { kMenu, kCalibrating, kPlaying, kPaused, kComplete, kUnsupported };

enum class ObjectFrame : uint8_t {
    kMarble = 0,
    kGoal0 = 1,
    kGoal1 = 2,
    kGoal2 = 3,
    kGoal3 = 4,
    kStar0 = 5,
    kStar1 = 6,
    kStar2 = 7,
    kStar3 = 8,
    kPit = 9,
    kBumper0 = 10,
    kBumper1 = 11,
    kBumper2 = 12,
    kSpark0 = 13,
    kSpark1 = 14,
    kSpark2 = 15,
};

enum class MechanicFrame : uint8_t {
    kBlockerHorizontal = 0,
    kBlockerVertical = 1,
    kGateHorizontal = 2,
    kGateVertical = 3,
    kPressurePlateOff = 4,
    kPressurePlateOn = 5,
    kPortalA = 6,
    kPortalB = 7,
};

enum class BoardTileFrame : uint8_t {
    kWallBlock = 0,
    kIce = 1,
    kAirflowNorth = 2,
    kAirflowNorthEast = 3,
    kAirflowEast = 4,
    kAirflowSouthEast = 5,
    kAirflowSouth = 6,
    kAirflowSouthWest = 7,
    kAirflowWest = 8,
    kAirflowNorthWest = 9,
};

struct Vec2 final {
    float x{};
    float y{};
};

struct PointF final {
    float x{};
    float y{};
};

struct WallRect final {
    int16_t x{};
    int16_t y{};
    int16_t width{};
    int16_t height{};
};

struct WallJointFeature final {
    int16_t x{};
    int16_t y{};
    uint8_t directions{};
};

struct VisualWallFeature final {
    WallRect rect{};
    uint8_t horizontal{};
    uint8_t start_connected{};
    uint8_t end_connected{};
};

struct CircleFeature final {
    int16_t x{};
    int16_t y{};
    int16_t radius{};
};

struct RectFeature final {
    int16_t x{};
    int16_t y{};
    int16_t width{};
    int16_t height{};
};

struct FanFeature final {
    int16_t x{};
    int16_t y{};
    int16_t radius{};
    int16_t force_x{};
    int16_t force_y{};
    uint16_t period_ms{};
    uint16_t active_ms{};
    uint16_t phase_ms{};
};

struct MovingWallFeature final {
    WallRect start{};
    PointF end{};
    uint32_t period_ms{};
};

struct TimedGateFeature final {
    WallRect rect{};
    uint32_t period_ms{};
    uint32_t open_ms{};
    uint32_t phase_ms{};
};

struct PressureGateFeature final {
    CircleFeature plate{};
    WallRect gate{};
};

struct PortalPairFeature final {
    CircleFeature first{};   // One-way trap entrance.
    CircleFeature second{};  // Safe return destination.
};

using Line = micropixel::FixedString<96U>;

[[nodiscard]] constexpr float ClampFloat(float value, float minimum, float maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

[[nodiscard]] constexpr int32_t RoundToInt(float value) {
    return static_cast<int32_t>(value >= 0.0F ? value + 0.5F : value - 0.5F);
}

}  // namespace tilt

#endif
