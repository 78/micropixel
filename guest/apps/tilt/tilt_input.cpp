#include "apps/tilt/tilt_input.hpp"

namespace tilt {
namespace {

constexpr uint32_t kCalibrationSampleCount = 18U;
constexpr float kFilterAlpha = 0.18F;
constexpr float kFullTiltMetersPerSecondSquared = 1.9F;
constexpr float kDeadZone = 0.06F;

}  // namespace

bool TiltInput::Initialize(micropixel::Application& app) {
    auto listed = app.devices().List(micropixel::DeviceKind::kSensor);
    if (!listed.has_value()) {
        return false;
    }
    for (micropixel::DeviceId device : listed.value()) {
        auto info = app.sensors().GetInfo(device);
        if (!info.has_value() || info->kind != micropixel::SensorKind::kAcceleration) {
            continue;
        }
        auto opened = app.sensors().Open<micropixel::Acceleration>(device);
        if (!opened.has_value()) {
            continue;
        }
        accelerometer_ = static_cast<micropixel::Accelerometer&&>(opened.value());
        auto configured = accelerometer_.SetSampleInterval(micropixel::Duration::Milliseconds(10U));
        if (!configured.has_value()) {
            accelerometer_.Reset();
            continue;
        }
        Recalibrate();
        return true;
    }
    return false;
}

void TiltInput::Recalibrate() {
    calibration_sum_ = {};
    neutral_ = {};
    filtered_ = {};
    last_sample_ = {};
    tilt_ = {};
    calibration_samples_ = 0U;
    calibrated_ = false;
    filter_seeded_ = false;
}

float TiltInput::ApplyDeadZone(float value) {
    const float magnitude = value < 0.0F ? -value : value;
    if (magnitude <= kDeadZone) {
        return 0.0F;
    }
    const float scaled = (magnitude - kDeadZone) / (1.0F - kDeadZone);
    return value < 0.0F ? -scaled : scaled;
}

bool TiltInput::Sample() {
    if (!available()) {
        return false;
    }
    auto result = accelerometer_.Read();
    if (!result.has_value()) {
        return result.error().code() == micropixel::ErrorCode::kWouldBlock;
    }
    if (last_sample_.microseconds() == result->timestamp.microseconds()) {
        return true;
    }
    last_sample_ = result->timestamp;
    const micropixel::Vector3 value = result->value.meters_per_second_squared;
    if (!calibrated_) {
        calibration_sum_.x += value.x;
        calibration_sum_.y += value.y;
        calibration_sum_.z += value.z;
        ++calibration_samples_;
        if (calibration_samples_ >= kCalibrationSampleCount) {
            const float inverse = 1.0F / static_cast<float>(calibration_samples_);
            neutral_ = {calibration_sum_.x * inverse, calibration_sum_.y * inverse, calibration_sum_.z * inverse};
            filtered_ = neutral_;
            filter_seeded_ = true;
            calibrated_ = true;
        }
        return true;
    }
    if (!filter_seeded_) {
        filtered_ = value;
        filter_seeded_ = true;
    } else {
        filtered_.x += (value.x - filtered_.x) * kFilterAlpha;
        filtered_.y += (value.y - filtered_.y) * kFilterAlpha;
        filtered_.z += (value.z - filtered_.z) * kFilterAlpha;
    }
    // ESP-Mosaico hardware validation established that the sensor X axis runs
    // opposite to screen X, while sensor Y already follows screen Y.
    tilt_.x = ApplyDeadZone(ClampFloat((neutral_.x - filtered_.x) / kFullTiltMetersPerSecondSquared, -1.0F, 1.0F));
    tilt_.y = ApplyDeadZone(ClampFloat((filtered_.y - neutral_.y) / kFullTiltMetersPerSecondSquared, -1.0F, 1.0F));
    return true;
}

}  // namespace tilt
