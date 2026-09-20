#ifndef MICROPIXEL_SDK_TILT_FILTER_HPP
#define MICROPIXEL_SDK_TILT_FILTER_HPP

#include <stdint.h>

#include "sdk/math.hpp"
#include "sdk/sensor_types.hpp"
#include "sdk/types.hpp"

namespace micropixel {

struct TiltFilterConfig final {
    // Samples averaged to establish the resting orientation after Recalibrate().
    uint32_t calibration_samples{18U};
    // Exponential low-pass weight per accepted sample (0 < alpha <= 1).
    float filter_alpha{0.18F};
    // Acceleration difference from rest that reads as full deflection, m/s^2.
    float full_scale{1.9F};
    // Fraction of full deflection ignored around rest.
    float deadzone{0.06F};
    // Sensor axis directions relative to the screen. Boards validated so far
    // (ESP-Mosaico) report sensor +X opposite to screen X and +Y along it.
    bool invert_x{true};
    bool invert_y{false};
};

// Turns raw accelerometer samples into a -1..1 screen-space tilt: calibrates
// a neutral orientation, low-pass filters, applies a deadzone. Games only
// call Sample() with each new reading and read tilt(). Motion games that
// need a gravity-relative frame or gyro aim build on math::ApplyDeadzone
// directly (see maze-evil's MotionControls).
//
//   micropixel::TiltFilter tilt;                 // default config
//   auto sample = accelerometer.Read();          // Result<SensorSample<Acceleration>>
//   if (sample) tilt.Sample(sample->value, sample->timestamp);
//   if (tilt.calibrated()) ball.Accelerate(tilt.x(), tilt.y());
class TiltFilter final {
   public:
    TiltFilter() = default;
    constexpr explicit TiltFilter(const TiltFilterConfig& config) : config_(config) {}

    [[nodiscard]] constexpr const TiltFilterConfig& config() const { return config_; }

    // Forgets the neutral orientation; the next `calibration_samples` distinct
    // samples define it again. Call when the game (re)starts or the player
    // asks to re-centre.
    void Recalibrate() {
        calibration_sum_ = {};
        neutral_ = {};
        filtered_ = {};
        tilt_x_ = 0.0F;
        tilt_y_ = 0.0F;
        calibration_count_ = 0U;
        calibrated_ = false;
        seeded_ = false;
        last_sample_ = {};
    }

    // Feeds one reading. Repeated timestamps (the Host cache has not refreshed)
    // are ignored. Returns true when the reading advanced the filter.
    bool Sample(const Acceleration& reading, TimePoint timestamp) {
        if (seeded_ && timestamp == last_sample_) {
            return false;
        }
        last_sample_ = timestamp;
        const Vector3 value = reading.meters_per_second_squared;
        if (!calibrated_) {
            calibration_sum_.x += value.x;
            calibration_sum_.y += value.y;
            calibration_sum_.z += value.z;
            ++calibration_count_;
            seeded_ = true;
            if (calibration_count_ >= math::Max(config_.calibration_samples, 1U)) {
                const float inverse = 1.0F / static_cast<float>(calibration_count_);
                neutral_ = {calibration_sum_.x * inverse, calibration_sum_.y * inverse, calibration_sum_.z * inverse};
                filtered_ = neutral_;
                calibrated_ = true;
            }
            return true;
        }
        const float alpha = math::Clamp(config_.filter_alpha, 0.01F, 1.0F);
        filtered_.x += (value.x - filtered_.x) * alpha;
        filtered_.y += (value.y - filtered_.y) * alpha;
        filtered_.z += (value.z - filtered_.z) * alpha;
        const float scale = config_.full_scale > 0.0F ? config_.full_scale : 1.0F;
        const float raw_x = (filtered_.x - neutral_.x) / scale;
        const float raw_y = (filtered_.y - neutral_.y) / scale;
        tilt_x_ = math::ApplyDeadzone(math::Clamp(config_.invert_x ? -raw_x : raw_x, -1.0F, 1.0F), config_.deadzone);
        tilt_y_ = math::ApplyDeadzone(math::Clamp(config_.invert_y ? -raw_y : raw_y, -1.0F, 1.0F), config_.deadzone);
        return true;
    }

    [[nodiscard]] constexpr bool calibrated() const { return calibrated_; }
    // Calibration progress in 0..1 for a "hold still" indicator.
    [[nodiscard]] constexpr float calibration_progress() const {
        const uint32_t needed = config_.calibration_samples == 0U ? 1U : config_.calibration_samples;
        return calibrated_ ? 1.0F : static_cast<float>(calibration_count_) / static_cast<float>(needed);
    }
    // Screen-space tilt, -1..1; positive x = screen right, positive y = screen down.
    [[nodiscard]] constexpr float x() const { return tilt_x_; }
    [[nodiscard]] constexpr float y() const { return tilt_y_; }
    [[nodiscard]] constexpr Vector3 neutral() const { return neutral_; }
    [[nodiscard]] constexpr Vector3 filtered() const { return filtered_; }

   private:
    TiltFilterConfig config_{};
    Vector3 calibration_sum_{};
    Vector3 neutral_{};
    Vector3 filtered_{};
    TimePoint last_sample_{};
    float tilt_x_{};
    float tilt_y_{};
    uint32_t calibration_count_{};
    bool calibrated_{};
    bool seeded_{};
};

}  // namespace micropixel

#endif
