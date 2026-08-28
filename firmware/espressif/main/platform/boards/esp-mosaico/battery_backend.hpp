#pragma once

#include <atomic>

#include "device/battery.hpp"
#include "driver/i2c_master.h"
#include "platform/common/i2c_executor.hpp"
#include "platform/drivers/power/bq27220.hpp"

namespace micropixel::platform::esp_mosaico {

class BatteryBackend final : public device::BatteryBackend {
   public:
    void Initialize(i2c_master_bus_handle_t bus, common::I2cExecutor& executor);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;
    void SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) override;

   private:
    [[nodiscard]] device::BatterySnapshot RefreshOnWorker();

    common::I2cExecutor* executor_{};
    drivers::Bq27220 fuel_gauge_{};
    device::BatterySnapshot last_snapshot_{};
    std::atomic<device::BatteryStateChangeSink> state_change_sink_{};
    std::atomic<void*> state_change_context_{};
};

}  // namespace micropixel::platform::esp_mosaico
