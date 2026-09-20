#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_SENSOR_PERIPHERAL_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_SENSOR_PERIPHERAL_HPP

#include <array>

#include "device/contracts/sensors.hpp"
#include "driver/i2c_master.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/bmi270.hpp"
#include "platform/drivers/sensors/bmm150.hpp"
#include "platform/sensors/polled_vector_sensor_peripheral.hpp"

namespace micropixel::platform::esp_mosaico {

class SensorPeripheral final : public device::SensorPeripheral {
   public:
    static constexpr device::PeripheralChannelId kAcceleration = 1U;
    static constexpr device::PeripheralChannelId kAngularVelocity = 2U;
    static constexpr device::PeripheralChannelId kMagneticField2 = 3U;
    static constexpr device::PeripheralChannelId kMagneticField3 = 4U;

    ~SensorPeripheral() override;

    [[nodiscard]] esp_err_t BeginInitialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor);
    [[nodiscard]] esp_err_t FinishInitialize();

    [[nodiscard]] bool acceleration_available() const { return acceleration_.available(); }
    [[nodiscard]] bool angular_velocity_available() const { return angular_velocity_.available(); }
    [[nodiscard]] bool magnetic_field2_available() const { return magnetic_field2_.available(); }
    [[nodiscard]] bool magnetic_field3_available() const { return magnetic_field3_.available(); }
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel,
                                  micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(device::PeripheralChannelId channel, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, device::SensorValues& values_out) override;
    void Stop(device::PeripheralChannelId channel) override;

   private:
    void InitializeOnWorker();

    i2c_master_bus_handle_t bus_{};
    drivers::Bmi270 inertial_{};
    drivers::Bmi270Vector acceleration_{inertial_, drivers::Bmi270::Kind::kAcceleration};
    drivers::Bmi270Vector angular_velocity_{inertial_, drivers::Bmi270::Kind::kAngularVelocity};
    drivers::Bmm150 magnetic_field2_{0x11U};
    drivers::Bmm150 magnetic_field3_{0x12U};
    buses::I2cExecutor* i2c_executor_{};
    bool initialization_started_{};
    bool initialization_finished_{};
    std::array<sensors::PolledVectorSensorPeripheral::Channel, 4> channels_{{
        {kAcceleration, MICROPIXEL_SENSOR_ACCELERATION, acceleration_, "mosaico_accel"},
        {kAngularVelocity, MICROPIXEL_SENSOR_ANGULAR_VELOCITY, angular_velocity_, "mosaico_gyro"},
        {kMagneticField2, MICROPIXEL_SENSOR_MAGNETIC_FIELD, magnetic_field2_, "mosaico_mag2"},
        {kMagneticField3, MICROPIXEL_SENSOR_MAGNETIC_FIELD, magnetic_field3_, "mosaico_mag3"},
    }};
    sensors::PolledVectorSensorPeripheral peripheral_{channels_, "mosaico_sensors"};
};

}  // namespace micropixel::platform::esp_mosaico

#endif
