#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_SENSOR_PERIPHERAL_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_SENSOR_PERIPHERAL_HPP

#include <atomic>

#include "device/contracts/sensors.hpp"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/bmi270.hpp"
#include "platform/drivers/sensors/bmm150.hpp"

namespace micropixel::platform::esp_mosaico {

class SensorPeripheral final : public device::SensorPeripheral {
   public:
    static constexpr device::PeripheralChannelId kAcceleration = 1U;
    static constexpr device::PeripheralChannelId kAngularVelocity = 2U;
    static constexpr device::PeripheralChannelId kMagneticField2 = 3U;
    static constexpr device::PeripheralChannelId kMagneticField3 = 4U;

    ~SensorPeripheral() override;

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor);

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
    struct Sampler final {
        SensorPeripheral* owner{};
        device::PeripheralChannelId channel{};
        esp_timer_handle_t timer{};
        device::SensorValues latest{};
        std::atomic<bool> active{};
        std::atomic<bool> pending{};
        int32_t status{MICROPIXEL_STATUS_WOULD_BLOCK};
    };

    void InitializeOnWorker();
    [[nodiscard]] int32_t ReadVector(device::PeripheralChannelId channel, device::SensorValues& values_out);
    [[nodiscard]] int32_t ConfigureOnWorker(device::PeripheralChannelId channel, uint32_t interval_us);
    void StopOnWorker(device::PeripheralChannelId channel);
    [[nodiscard]] drivers::VectorSensor* DriverFor(device::PeripheralChannelId channel);
    [[nodiscard]] Sampler* FindSampler(device::PeripheralChannelId channel);
    [[nodiscard]] const Sampler* FindSampler(device::PeripheralChannelId channel) const;
    static void TimerExpired(void* context);
    static esp_err_t SampleOnWorker(void* context);

    i2c_master_bus_handle_t bus_{};
    drivers::Bmi270 inertial_{};
    drivers::Bmi270Vector acceleration_{inertial_, drivers::Bmi270::Kind::kAcceleration};
    drivers::Bmi270Vector angular_velocity_{inertial_, drivers::Bmi270::Kind::kAngularVelocity};
    drivers::Bmm150 magnetic_field2_{0x11U};
    drivers::Bmm150 magnetic_field3_{0x12U};
    buses::I2cExecutor* i2c_executor_{};
    Sampler acceleration_sampler_{};
    Sampler angular_velocity_sampler_{};
    Sampler magnetic_field2_sampler_{};
    Sampler magnetic_field3_sampler_{};
    portMUX_TYPE cache_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace micropixel::platform::esp_mosaico

#endif
