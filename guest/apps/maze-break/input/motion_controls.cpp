#include "apps/maze-break/input/motion_controls.hpp"

#include "apps/maze-break/rc_math.hpp"
#include "sdk/devices.hpp"

namespace maze_break::input {
namespace {

constexpr int kCalibrationSamples = 25;  // ~0.5 s of distinct 100 Hz samples at 40 fps
constexpr float kFilterAlpha = 0.25F;    // accelerometer low-pass
constexpr float kFullTilt = 0.42F;       // fraction of g for full deflection (~25 deg)
constexpr float kDeadzone = 0.07F;       // fraction of full deflection
constexpr float kGyroAimGain = 1.0F;     // view radians per device radian; 0 disables
constexpr float kGyroDeadband = 0.03F;   // rad/s, hides bias drift
constexpr uint64_t kSampleIntervalUs = 10'000U;

float ApplyDeadzone(float value) {
    const float magnitude = math::Fabs(value);
    if (magnitude < kDeadzone) {
        return 0.0F;
    }
    const float scaled = (magnitude - kDeadzone) / (1.0F - kDeadzone);
    return value < 0.0F ? -scaled : scaled;
}

// Softer response near the centre for fine aiming, full rate at the edge.
float Expo(float v) { return 0.45F * v + 0.55F * v * v * v; }

template <typename Reading>
bool OpenFirst(micropixel::Application& app, micropixel::SensorKind kind, micropixel::Sensor<Reading>& out) {
    auto listed = app.devices().List(micropixel::DeviceKind::kSensor);
    if (!listed.has_value()) {
        return false;
    }
    for (micropixel::DeviceId device : listed.value()) {
        auto info = app.sensors().GetInfo(device);
        if (!info.has_value() || info->kind != kind) {
            continue;
        }
        auto opened = app.sensors().Open<Reading>(device);
        if (!opened.has_value()) {
            continue;
        }
        out = static_cast<micropixel::Sensor<Reading>&&>(opened.value());
        if (!out.SetSampleInterval(micropixel::Duration::Microseconds(kSampleIntervalUs)).has_value()) {
            out.Reset();
            continue;
        }
        return true;
    }
    return false;
}

}  // namespace

bool MotionControls::Initialize(micropixel::Application& app) {
    if (!OpenFirst(app, micropixel::SensorKind::kAcceleration, accelerometer_)) {
        return false;
    }
    // The gyroscope is optional: without it aiming falls back to roll only.
    (void)OpenFirst(app, micropixel::SensorKind::kAngularVelocity, gyroscope_);
    Recalibrate();
    return true;
}

void MotionControls::Recalibrate() { recalibrate_request_ = true; }

bool MotionControls::BuildNeutralFrame() {
    // Sensor frame (validated by the Tilt app's axis mapping): +X = screen
    // right, +Y = screen top, +Z = out of the screen. From the neutral up
    // vector derive the horizontal "right" and "back" (towards the viewer /
    // bottom edge) axes: right_ = X made orthogonal to up, back_ = right_ x up.
    const float norm = math::Sqrt(neutral_[0] * neutral_[0] + neutral_[1] * neutral_[1] + neutral_[2] * neutral_[2]);
    if (norm < 1.0F) {
        return false;
    }
    const float up[3] = {neutral_[0] / norm, neutral_[1] / norm, neutral_[2] / norm};
    const float x_dot_up = up[0];  // dot(+X, up)
    const float right[3] = {1.0F - x_dot_up * up[0], -x_dot_up * up[1], -x_dot_up * up[2]};
    const float right_norm = math::Sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (right_norm < 0.3F) {
        return false;  // screen edge-on: no usable horizontal "right"
    }
    for (int i = 0; i < 3; ++i) {
        right_[i] = right[i] / right_norm;
    }
    back_[0] = right_[1] * up[2] - right_[2] * up[1];
    back_[1] = right_[2] * up[0] - right_[0] * up[2];
    back_[2] = right_[0] * up[1] - right_[1] * up[0];
    return true;
}

void MotionControls::Poll() {
    if (!accelerometer_.valid()) {
        return;
    }
    if (gyroscope_.valid()) {
        auto gyro = gyroscope_.Read();
        if (gyro.has_value() && gyro->timestamp.microseconds() != last_gyro_us_) {
            last_gyro_us_ = gyro->timestamp.microseconds();
            latest_gyro_[0] = gyro->value.radians_per_second.x;
            latest_gyro_[1] = gyro->value.radians_per_second.y;
            latest_gyro_[2] = gyro->value.radians_per_second.z;
        }
    }
    auto accel = accelerometer_.Read();
    if (!accel.has_value() || accel->timestamp.microseconds() == last_accel_us_) {
        return;  // no new sample yet
    }
    const uint64_t now_us = accel->timestamp.microseconds();
    const float dt = last_accel_us_ == 0U ? 0.01F : static_cast<float>(now_us - last_accel_us_) * 1e-6F;
    last_accel_us_ = now_us;
    const float sample[3] = {accel->value.meters_per_second_squared.x, accel->value.meters_per_second_squared.y,
                             accel->value.meters_per_second_squared.z};
    Integrate(sample, latest_gyro_, dt);
}

void MotionControls::Integrate(const float accel[3], const float gyro[3], float dt) {
    if (recalibrate_request_) {
        recalibrate_request_ = false;
        calibration_left_ = kCalibrationSamples;
        for (float& v : calibration_sum_) {
            v = 0.0F;
        }
        calibrated_ = false;
    }
    if (calibration_left_ > 0) {
        for (int i = 0; i < 3; ++i) {
            calibration_sum_[i] += accel[i];
            calibration_sum_[3 + i] += gyro[i];
        }
        if (--calibration_left_ == 0) {
            for (int i = 0; i < 3; ++i) {
                neutral_[i] = calibration_sum_[i] / kCalibrationSamples;
                gyro_bias_[i] = calibration_sum_[3 + i] / kCalibrationSamples;
                filtered_[i] = neutral_[i];
            }
            filter_seeded_ = true;
            calibrated_ = BuildNeutralFrame();
            if (!calibrated_) {
                // Held edge-on; try again with the next batch.
                calibration_left_ = kCalibrationSamples;
                for (float& v : calibration_sum_) {
                    v = 0.0F;
                }
            }
        }
        forward_ = 0.0F;
        turn_rate_ = 0.0F;
        return;
    }

    if (!filter_seeded_) {
        for (int i = 0; i < 3; ++i) {
            filtered_[i] = accel[i];
        }
        filter_seeded_ = true;
    } else {
        for (int i = 0; i < 3; ++i) {
            filtered_[i] += (accel[i] - filtered_[i]) * kFilterAlpha;
        }
    }

    // Current "up" unit vector in the sensor frame.
    const float norm =
        math::Sqrt(filtered_[0] * filtered_[0] + filtered_[1] * filtered_[1] + filtered_[2] * filtered_[2]);
    if (norm < 1.0F) {
        return;  // free fall / sensor glitch; keep the previous controls
    }
    float up[3];
    for (int i = 0; i < 3; ++i) {
        up[i] = filtered_[i] / norm;
    }

    // Tilt is measured against the two horizontal axes captured at
    // calibration, so the same gestures work whether the board lies flat or is
    // held upright. The up vector leans towards the raised edge: lowering the
    // right edge gives a negative component along right_, pushing the top edge
    // away gives a positive component along back_.
    const float lean_right = up[0] * right_[0] + up[1] * right_[1] + up[2] * right_[2];
    const float lean_back = up[0] * back_[0] + up[1] * back_[1] + up[2] * back_[2];
    const float tilt_turn = ApplyDeadzone(math::Clamp(-lean_right / kFullTilt, -1.0F, 1.0F));
    const float tilt_forward = ApplyDeadzone(math::Clamp(lean_back / kFullTilt, -1.0F, 1.0F));

    // Gyro aim: rotation about the current gravity axis. A clockwise turn seen
    // from above (looking right) is a negative right-hand-rule rotation.
    float yaw_delta = 0.0F;
    if (gyroscope_.valid() && kGyroAimGain != 0.0F) {
        float yaw_rate = 0.0F;
        for (int i = 0; i < 3; ++i) {
            yaw_rate += (gyro[i] - gyro_bias_[i]) * up[i];
        }
        if (math::Fabs(yaw_rate) > kGyroDeadband) {
            yaw_delta = -yaw_rate * dt * kGyroAimGain;
        }
    }

    forward_ = tilt_forward;
    turn_rate_ = Expo(tilt_turn);
    yaw_accum_ += yaw_delta;
}

MotionControls::Sample MotionControls::Consume() {
    Sample out{};
    out.forward = forward_;
    out.turn_rate = turn_rate_;
    out.yaw_delta = yaw_accum_;
    yaw_accum_ = 0.0F;
    return out;
}

}  // namespace maze_break::input
