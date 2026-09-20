// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <span>

#include "device/contracts/sensors.hpp"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/sensors/vector_sensor.hpp"

namespace micropixel::platform::sensors {

// Borrows a fixed board-owned channel array. Drivers, channels and the executor
// must outlive this object; lifecycle calls run outside the I2C worker.
class PolledVectorSensorPeripheral final : public device::SensorPeripheral {
   public:
    class Channel final {
       public:
        Channel(device::PeripheralChannelId id, uint16_t kind, drivers::VectorSensor& driver, const char* timer_name)
            : id_(id), kind_(kind), driver_(driver), timer_name_(timer_name) {}

       private:
        friend class PolledVectorSensorPeripheral;
        device::PeripheralChannelId id_;
        uint16_t kind_;
        drivers::VectorSensor& driver_;
        const char* timer_name_;
        PolledVectorSensorPeripheral* owner_{};
        esp_timer_handle_t timer_{};
        device::SensorValues latest_{};
        std::atomic<bool> active_{};
        std::atomic<bool> pending_{};
        int32_t status_{MICROPIXEL_STATUS_WOULD_BLOCK};
    };

    PolledVectorSensorPeripheral(std::span<Channel> channels, const char* log_tag)
        : channels_(channels), log_tag_(log_tag) {}
    ~PolledVectorSensorPeripheral() override;

    // Drivers are initialized by the board before timers are prepared. Failure
    // to create one timer leaves other channels usable, as with absent drivers.
    [[nodiscard]] esp_err_t Initialize(buses::I2cExecutor& executor);
    [[nodiscard]] int32_t GetInfo(device::PeripheralChannelId channel,
                                  micropixel_sensor_info_t& info_out) const override;
    [[nodiscard]] int32_t Start(device::PeripheralChannelId channel, uint32_t interval_us) override;
    [[nodiscard]] int32_t Read(device::PeripheralChannelId channel, device::SensorValues& values_out) override;
    void Stop(device::PeripheralChannelId channel) override;

   private:
    [[nodiscard]] Channel* FindChannel(device::PeripheralChannelId channel) const;
    [[nodiscard]] esp_err_t Quiesce(Channel& channel);
    void ResetCache(Channel& channel);
    static void TimerExpired(void* context);
    static esp_err_t SampleOnWorker(void* context);

    std::span<Channel> channels_;
    const char* log_tag_;
    buses::I2cExecutor* executor_{};
    portMUX_TYPE cache_lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace micropixel::platform::sensors
