#ifndef MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_SENSOR_PERIPHERAL_HPP
#define MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_SENSOR_PERIPHERAL_HPP

#include <atomic>

#include "device/contracts/sensors.hpp"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/qmc6309.hpp"
#include "platform/drivers/sensors/sc7a20htr.hpp"

namespace micropixel::platform::metalio_claw4 {

class SensorPeripheral final : public device::SensorPeripheral {
   public:
    static constexpr device::PeripheralChannelId kAcceleration = 1U;
    static constexpr device::PeripheralChannelId kMagneticField = 2U;

    ~SensorPeripheral() override;

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& i2c_executor);

    [[nodiscard]] bool acceleration_available() const { return acceleration_.available(); }
    [[nodiscard]] bool magnetic_field_available() const { return magnetic_field_.available(); }
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
    [[nodiscard]] int32_t ReadAcceleration(device::SensorValues& values_out);
    [[nodiscard]] int32_t ReadMagneticField(device::SensorValues& values_out);
    [[nodiscard]] int32_t ConfigureOnWorker(device::PeripheralChannelId channel, uint32_t interval_us);
    void StopOnWorker(device::PeripheralChannelId channel);
    [[nodiscard]] drivers::VectorSensor* DriverFor(device::PeripheralChannelId channel);
    [[nodiscard]] Sampler* FindSampler(device::PeripheralChannelId channel);
    [[nodiscard]] const Sampler* FindSampler(device::PeripheralChannelId channel) const;
    static void TimerExpired(void* context);
    static esp_err_t SampleOnWorker(void* context);

    i2c_master_bus_handle_t bus_{};
    drivers::Sc7a20htr acceleration_{};
    drivers::Qmc6309 magnetic_field_{};
    buses::I2cExecutor* i2c_executor_{};
    Sampler acceleration_sampler_{};
    Sampler magnetic_field_sampler_{};
    portMUX_TYPE cache_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace micropixel::platform::metalio_claw4

#endif
