#ifndef MICROPIXEL_TILT_INPUT_HPP
#define MICROPIXEL_TILT_INPUT_HPP

#include "apps/tilt/tilt_common.hpp"

namespace tilt {

class TiltInput final {
   public:
    bool Initialize(micropixel::Application& app);
    bool Sample();
    void Recalibrate();

    [[nodiscard]] constexpr bool available() const { return accelerometer_.valid(); }
    [[nodiscard]] constexpr bool calibrated() const { return calibrated_; }
    [[nodiscard]] constexpr Vec2 tilt() const { return tilt_; }

   private:
    static float ApplyDeadZone(float value);

    micropixel::Accelerometer accelerometer_{};
    micropixel::Vector3 calibration_sum_{};
    micropixel::Vector3 neutral_{};
    micropixel::Vector3 filtered_{};
    micropixel::TimePoint last_sample_{};
    Vec2 tilt_{};
    uint32_t calibration_samples_{};
    bool calibrated_{};
    bool filter_seeded_{};
};

}  // namespace tilt

#endif
