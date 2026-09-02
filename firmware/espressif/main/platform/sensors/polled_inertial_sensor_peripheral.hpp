#pragma once

#include <atomic>

#include "device/contracts/sensors.hpp"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/vector_sensor.hpp"

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
    ~PolledInertialSensorPeripheral() override;

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor);

    [[nodiscard]] bool acceleration_available() const { return acceleration_.available(); }
    [[nodiscard]] bool angular_velocity_available() const { return angular_velocity_.available(); }
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel,
                                  micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(device::PeripheralChannelId channel, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, device::SensorValues& values_out) override;
    void Stop(device::PeripheralChannelId channel) override;

   private:
    struct Sampler final {
        PolledInertialSensorPeripheral* owner{};
        device::PeripheralChannelId channel{};
        esp_timer_handle_t timer{};
        device::SensorValues latest{};
        std::atomic<bool> active{};
        std::atomic<bool> pending{};
        int32_t status{MICROPIXEL_STATUS_WOULD_BLOCK};
    };

    void InitializeOnWorker(i2c_master_bus_handle_t bus);
    [[nodiscard]] int32_t ConfigureOnWorker(device::PeripheralChannelId channel, uint32_t interval_us);
    [[nodiscard]] int32_t ReadVector(device::PeripheralChannelId channel, device::SensorValues& values_out);
    void StopOnWorker(device::PeripheralChannelId channel);
    [[nodiscard]] drivers::VectorSensor* DriverFor(device::PeripheralChannelId channel);
    [[nodiscard]] Sampler* FindSampler(device::PeripheralChannelId channel);
    [[nodiscard]] const Sampler* FindSampler(device::PeripheralChannelId channel) const;
    static void TimerExpired(void* context);
    static esp_err_t SampleOnWorker(void* context);

    drivers::VectorSensor& acceleration_;
    drivers::VectorSensor& angular_velocity_;
    PolledInertialSensorConfig config_;
    buses::I2cExecutor* i2c_executor_{};
    Sampler acceleration_sampler_{};
    Sampler angular_velocity_sampler_{};
    portMUX_TYPE cache_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace micropixel::platform::sensors
