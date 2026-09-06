#ifndef MICROPIXEL_APPS_MAZE_BREAK_INPUT_MOTION_CONTROLS_HPP
#define MICROPIXEL_APPS_MAZE_BREAK_INPUT_MOTION_CONTROLS_HPP

#include <stdint.h>

#include "sdk/application.hpp"
#include "sdk/sensors.hpp"

namespace maze_break::input {

// Motion control from the board IMU through the Sensors service:
//   pitch (tilt the top edge away / towards you)   -> walk forward / back
//   roll  (lower the right / left edge)            -> turn at a rate, like a stick
//   yaw   (swing the whole device left / right)    -> gyro aim, 1:1 with the view
//
// The neutral orientation is captured after the start-screen confirmation, on every restart and when
// the function key is held for 1.5 s. Tilt is measured against horizontal axes
// derived from that neutral pose, so the device can be played lying flat, held
// upright like a phone, or anywhere in between.
//
// Sensors are polled once per frame; the Host samples them at 100 Hz and
// Read() hands back the newest value.
class MotionControls final {
   public:
    struct Sample {
        float forward{};    // -1..1
        float turn_rate{};  // -1..1, positive = right
        float yaw_delta{};  // radians accumulated since the last Consume(), positive = right
    };

    // Opens the first accelerometer and (optionally) gyroscope. Returns false
    // when the board has no accelerometer; the game then stays in touch mode.
    [[nodiscard]] bool Initialize(micropixel::Application& app);
    // Pulls the newest sensor samples and updates the filtered controls.
    void Poll();
    [[nodiscard]] Sample Consume();
    // Restarts neutral-orientation capture (~0.5 s of samples).
    void Recalibrate();
    [[nodiscard]] bool ready() const { return calibrated_ && !recalibrate_request_; }
    [[nodiscard]] bool available() const { return accelerometer_.valid(); }  // NOLINT(readability-identifier-naming)

   private:
    // Derives right_/back_ from neutral_; false if the board is held edge-on.
    [[nodiscard]] bool BuildNeutralFrame();
    void Integrate(const float accel[3], const float gyro[3], float dt);

    micropixel::Accelerometer accelerometer_{};
    micropixel::Gyroscope gyroscope_{};
    uint64_t last_accel_us_{};
    uint64_t last_gyro_us_{};
    float latest_gyro_[3]{};

    // Calibration state.
    int calibration_left_{};
    float calibration_sum_[6]{};
    float neutral_[3]{};
    float right_[3]{};  // horizontal unit vector along the screen's right edge
    float back_[3]{};   // horizontal unit vector towards the player (bottom edge)
    float gyro_bias_[3]{};
    bool calibrated_{};
    float filtered_[3]{};
    bool filter_seeded_{};
    bool recalibrate_request_{true};

    float forward_{};
    float turn_rate_{};
    float yaw_accum_{};
};

}  // namespace maze_break::input

#endif
