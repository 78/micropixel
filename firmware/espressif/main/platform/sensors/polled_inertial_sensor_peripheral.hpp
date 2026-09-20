#pragma once

#include <array>

#include "device/contracts/sensors.hpp"
#include "driver/i2c_master.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/sensors/polled_vector_sensor_peripheral.hpp"

namespace micropixel::platform::sensors {

struct PolledInertialSensorConfig final {
    const char* log_tag;
    const char* model;
    const char* acceleration_timer_name;
    const char* angular_velocity_timer_name;
};

class PolledInertialSensorPeripheral final : public device::SensorPeripheral {
   public:
    static constexpr device::PeripheralChannelId kAcceleration = 1U;
    static constexpr device::PeripheralChannelId kAngularVelocity = 2U;

    PolledInertialSensorPeripheral(drivers::VectorSensor& acceleration, drivers::VectorSensor& angular_velocity,
                                   PolledInertialSensorConfig config);

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor);

    [[nodiscard]] bool acceleration_available() const { return acceleration_.available(); }
    [[nodiscard]] bool angular_velocity_available() const { return angular_velocity_.available(); }
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel,
                                  micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(device::PeripheralChannelId channel, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, device::SensorValues& values_out) override;
    void Stop(device::PeripheralChannelId channel) override;

   private:
    void InitializeOnWorker(i2c_master_bus_handle_t bus);

    drivers::VectorSensor& acceleration_;
    drivers::VectorSensor& angular_velocity_;
    PolledInertialSensorConfig config_;
    std::array<PolledVectorSensorPeripheral::Channel, 2> channels_;
    PolledVectorSensorPeripheral peripheral_;
};

}  // namespace micropixel::platform::sensors
