#pragma once

#include "device/contracts/sensors.hpp"
#include "driver/i2c_master.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/qmi8658.hpp"
#include "platform/sensors/polled_inertial_sensor_peripheral.hpp"

namespace micropixel::platform::szpi_esp32s3 {

class SensorPeripheral final : public device::SensorPeripheral {
   public:
    static constexpr device::PeripheralChannelId kAcceleration = sensors::PolledInertialSensorPeripheral::kAcceleration;
    static constexpr device::PeripheralChannelId kAngularVelocity =
        sensors::PolledInertialSensorPeripheral::kAngularVelocity;

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor);

    [[nodiscard]] bool acceleration_available() const { return peripheral_.acceleration_available(); }
    [[nodiscard]] bool angular_velocity_available() const { return peripheral_.angular_velocity_available(); }
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel,
                                  micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(device::PeripheralChannelId channel, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, device::SensorValues& values_out) override;
    void Stop(device::PeripheralChannelId channel) override;

   private:
    drivers::Qmi8658 inertial_{};
    drivers::Qmi8658::Vector acceleration_{inertial_, drivers::Qmi8658::Kind::kAcceleration};
    drivers::Qmi8658::Vector angular_velocity_{inertial_, drivers::Qmi8658::Kind::kAngularVelocity};
    sensors::PolledInertialSensorPeripheral peripheral_{acceleration_,
                                                        angular_velocity_,
                                                        {.log_tag = "szpi_sensors",
                                                         .model = "QMI8658",
                                                         .acceleration_timer_name = "szpi_accel",
                                                         .angular_velocity_timer_name = "szpi_gyro"}};
};

}  // namespace micropixel::platform::szpi_esp32s3
