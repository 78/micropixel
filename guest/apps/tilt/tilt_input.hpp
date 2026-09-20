#ifndef MICROPIXEL_TILT_INPUT_HPP
#define MICROPIXEL_TILT_INPUT_HPP

#include "apps/tilt/tilt_common.hpp"
#include "sdk/tilt_filter.hpp"

namespace tilt {

// The board accelerometer through the SDK TiltFilter: calibration, low-pass
// and deadzone live in the SDK; this class only owns the sensor handle and
// converts to the game's Vec2.
class TiltInput final {
   public:
    bool Initialize(micropixel::Application& app);
    bool Sample();
    void Recalibrate() { filter_.Recalibrate(); }

    [[nodiscard]] constexpr bool available() const { return accelerometer_.valid(); }
    [[nodiscard]] constexpr bool calibrated() const { return filter_.calibrated(); }
    [[nodiscard]] constexpr Vec2 tilt() const { return Vec2{filter_.x(), filter_.y()}; }

   private:
    micropixel::Accelerometer accelerometer_{};
    micropixel::TiltFilter filter_{};
};

}  // namespace tilt

#endif
