// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::m5stack_tab5 {

// Chip-to-device axis mapping for one inertial channel. The BMI270 is mounted
// rotated on this board, and the vendor BSP's table (M5Tab5-UserDemo
// hal_imu.cpp: acceleration = {+y, -x, -z}, angular velocity = {+y, +x, -z})
// describes the vendor's landscape canvas while this product runs the panel in
// portrait 720x1280. Exchanging the X and Y outputs of that table, with the
// remaining signs unchanged, is what the device validated on hardware: all
// three axes and both half-axes match the panel orientation.
class Tab5InertialAxis final : public drivers::VectorSensor {
   public:
    struct Axis final {
        uint8_t source;  // 0 = chip x, 1 = chip y, 2 = chip z
        bool inverted;
    };
    struct Mapping final {
        Axis x;
        Axis y;
        Axis z;
    };

    static constexpr Mapping kAccelerationMapping{{0U, true}, {1U, false}, {2U, true}};
    static constexpr Mapping kAngularVelocityMapping{{0U, false}, {1U, false}, {2U, true}};

    Tab5InertialAxis(drivers::VectorSensor& source, Mapping mapping) : source_(source), mapping_(mapping) {}

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus) override;
    [[nodiscard]] esp_err_t Configure(uint32_t interval_us) override;
    [[nodiscard]] esp_err_t Suspend() override;
    [[nodiscard]] esp_err_t Read(float (&values)[3]) override;
    [[nodiscard]] bool available() const override;
    [[nodiscard]] uint32_t min_interval_us() const override;

   private:
    drivers::VectorSensor& source_;
    Mapping mapping_{};
};

}  // namespace micropixel::platform::m5stack_tab5
