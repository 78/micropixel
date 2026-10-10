// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/battery.hpp"
#include "esp_timer.h"
#include "platform/buses/i2c_executor.hpp"
#include "platform/drivers/power/ina226.hpp"

namespace micropixel::platform::m5stack_tab5 {

class Pi4ioeExpander;

// Battery policy for this board. The INA226 sits on the pack input of a 2S
// NP-F550 module (7.4 V nominal, 6.0 V shutdown, 8.23 V full), and there is no
// fuel gauge: the percentage comes from a voltage table with hysteresis, while
// the current direction carries charging, discharging and the power source.
// IO expander 2 P6 is the IP2326 charge-status output, not a cable detect (see
// the schematic note in board_config.hpp); it only adds evidence that a source
// is attached, never evidence that one is missing.
class BatteryPeripheral final : public device::Battery {
   public:
    BatteryPeripheral() = default;
    BatteryPeripheral(const BatteryPeripheral&) = delete;
    BatteryPeripheral& operator=(const BatteryPeripheral&) = delete;
    ~BatteryPeripheral() override;

    void Initialize(i2c_master_bus_handle_t bus, Pi4ioeExpander& expander, buses::I2cExecutor& executor);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;
    void SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) override;

   private:
    [[nodiscard]] device::BatterySnapshot RefreshOnWorker();
    static void RefreshTimer(void* context);
    static esp_err_t RefreshEntry(void* context);
    void NotifyIfChanged(const device::BatterySnapshot& previous, const device::BatterySnapshot& current);

    drivers::Ina226 monitor_{};
    Pi4ioeExpander* expander_{};
    buses::I2cExecutor* executor_{};
    esp_timer_handle_t refresh_timer_{};
    device::BatterySnapshot last_snapshot_{};
    std::atomic<device::BatteryStateChangeSink> state_change_sink_{nullptr};
    std::atomic<void*> state_change_context_{nullptr};
    std::atomic<bool> refresh_pending_{false};
    bool sample_logged_{};
    // Power-source verdict, sticky between samples so a charger sitting on the
    // threshold cannot restart the idle timer. It starts at "external": a
    // board without a pack can only run from USB, and the idle power policy
    // needs a discharge before it is allowed to power the device off.
    bool external_power_connected_{true};
};

}  // namespace micropixel::platform::m5stack_tab5
