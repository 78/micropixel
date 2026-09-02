#pragma once

#include <atomic>

#include "device/contracts/battery.hpp"
#include "esp_timer.h"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::m5stack_cores3 {

class BoardHardware;

class BatteryPeripheral final : public device::Battery {
   public:
    ~BatteryPeripheral() override;

    void Initialize(BoardHardware& hardware, buses::I2cExecutor& executor);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;
    void SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) override;

   private:
    [[nodiscard]] device::BatterySnapshot RefreshOnWorker();
    static void RefreshTimer(void* context);
    static esp_err_t RefreshEntry(void* context);
    void NotifyIfChanged(const device::BatterySnapshot& previous, const device::BatterySnapshot& current);

    BoardHardware* hardware_{};
    buses::I2cExecutor* executor_{};
    device::BatterySnapshot last_snapshot_{};
    esp_timer_handle_t refresh_timer_{};
    bool sample_logged_{};
    std::atomic<bool> refresh_pending_{};
    std::atomic<device::BatteryStateChangeSink> state_change_sink_{};
    std::atomic<void*> state_change_context_{};
};

}  // namespace micropixel::platform::m5stack_cores3
