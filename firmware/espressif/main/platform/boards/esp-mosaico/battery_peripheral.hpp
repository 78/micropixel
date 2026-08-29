#pragma once

#include <atomic>

#include "device/contracts/battery.hpp"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/power/bq27220.hpp"

namespace micropixel::platform::esp_mosaico {

class BatteryPeripheral final : public device::Battery {
   public:
    ~BatteryPeripheral() override;

    void Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;
    void SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) override;

   private:
    [[nodiscard]] device::BatterySnapshot RefreshOnWorker();
    static void RefreshTimer(void* context);
    static esp_err_t RefreshEntry(void* context);
    void NotifyIfChanged(const device::BatterySnapshot& previous, const device::BatterySnapshot& current);

    buses::I2cExecutor* executor_{};
    drivers::Bq27220 fuel_gauge_{};
    device::BatterySnapshot last_snapshot_{};
    esp_timer_handle_t refresh_timer_{};
    std::atomic<bool> refresh_pending_{};
    std::atomic<device::BatteryStateChangeSink> state_change_sink_{};
    std::atomic<void*> state_change_context_{};
};

}  // namespace micropixel::platform::esp_mosaico
