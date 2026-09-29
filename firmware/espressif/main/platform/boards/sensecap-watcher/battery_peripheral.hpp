// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/battery.hpp"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_timer.h"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::sensecap_watcher {

class BoardPower;

// This board carries no fuel gauge. The pack voltage reaches the SoC through the
// vendor's 62k/20k divider on GPIO3 (ADC1 channel 2), and the state of charge is
// the vendor's curve fitted for the on-board 400 mAh cell. The charge state comes
// from the expander's VBUS_IN_DET line, which is the line the vendor UI and
// xiaozhi both use; the expander's BAT_DET presence line is logged but does not
// gate the level, because neither reference implementation consults it and a
// reporting unit whose detect line never asserts would otherwise show no level at
// all.
//
// A voltage-derived level reads high while the charger holds the terminal
// voltage up, so `percent` follows the pack's resting curve rather than a
// coulomb count; that is the accuracy this hardware supports.
//
// Sampling runs on the shared I2C executor. The expander lives on that bus and
// the executor's task has a stack that driver work expects, which the esp_timer
// task does not.
class BatteryPeripheral final : public device::Battery {
   public:
    ~BatteryPeripheral() override;

    [[nodiscard]] esp_err_t Initialize(BoardPower& power, buses::I2cExecutor& executor);
    [[nodiscard]] device::BatterySnapshot Snapshot() override;
    void SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) override;

   private:
    [[nodiscard]] device::BatterySnapshot Sample();
    [[nodiscard]] esp_err_t ReadPackMillivolts(uint32_t& millivolts);
    static void RefreshTimer(void* context);
    static esp_err_t RefreshEntry(void* context);
    void NotifyIfChanged(const device::BatterySnapshot& previous, const device::BatterySnapshot& current);

    BoardPower* power_{};
    buses::I2cExecutor* executor_{};
    adc_oneshot_unit_handle_t unit_{};
    adc_cali_handle_t calibration_{};
    device::BatterySnapshot last_snapshot_{};
    esp_timer_handle_t refresh_timer_{};
    bool sample_logged_{};
    bool range_warned_{};
    std::atomic<bool> refresh_pending_{};
    std::atomic<device::BatteryStateChangeSink> state_change_sink_{};
    std::atomic<void*> state_change_context_{};
};

}  // namespace micropixel::platform::sensecap_watcher
