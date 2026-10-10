// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/battery_peripheral.hpp"

#include <array>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "platform/boards/m5stack-tab5/board_config.hpp"
#include "platform/boards/m5stack-tab5/io_expander.hpp"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_battery";
constexpr uint64_t kRefreshIntervalUs = 2U * 1000U * 1000U;
// The pack is a 2S Li-ion module; outside this window the monitor is reading a
// charger output or nothing at all, so no battery is reported.
constexpr float kMinimumBatteryVolts = 5.0F;
constexpr float kMaximumBatteryVolts = 9.0F;
// Below this the current is treated as noise rather than a charge direction.
constexpr float kCurrentThresholdAmps = 0.05F;
constexpr uint8_t kHysteresisPercent = 3U;

// 2S open-circuit voltage table (volts, descending) for the NP-F550 module.
struct VoltageStep final {
    float volts;
    uint8_t percent;
};
constexpr std::array<VoltageStep, 12U> kVoltageTable{{
    {8.30F, 100U},
    {8.10F, 90U},
    {7.90F, 80U},
    {7.70F, 70U},
    {7.55F, 60U},
    {7.40F, 50U},
    {7.25F, 40U},
    {7.10F, 30U},
    {6.90F, 20U},
    {6.70F, 10U},
    {6.40F, 5U},
    {6.10F, 0U},
}};

[[nodiscard]] uint8_t PercentForVoltage(float volts) {
    for (const VoltageStep& step : kVoltageTable) {
        if (volts >= step.volts) {
            return step.percent;
        }
    }
    return 0U;
}

// Keeps the reported percentage from flickering while the pack rests under
// load: only a move of kHysteresisPercent is adopted.
[[nodiscard]] uint8_t ApplyHysteresis(uint8_t previous, uint8_t target) {
    const uint8_t difference =
        previous > target ? static_cast<uint8_t>(previous - target) : static_cast<uint8_t>(target - previous);
    return difference >= kHysteresisPercent ? target : previous;
}

}  // namespace

BatteryPeripheral::~BatteryPeripheral() {
    if (refresh_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(refresh_timer_, portMAX_DELAY);
        (void)esp_timer_delete(refresh_timer_);
    }
}

void BatteryPeripheral::Initialize(i2c_master_bus_handle_t bus, Pi4ioeExpander& expander,
                                   buses::I2cExecutor& executor) {
    expander_ = &expander;
    executor_ = &executor;
    const esp_err_t bind_status = monitor_.Bind(bus, {});
    if (bind_status != ESP_OK) {
        ESP_LOGW(kTag, "monitor bind failed: %s", esp_err_to_name(bind_status));
    }
    const esp_err_t status = executor.Invoke(buses::I2cExecutor::Priority::kLow, RefreshEntry, this);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "initial refresh failed: %s", esp_err_to_name(status));
    }
    esp_timer_create_args_t arguments{};
    arguments.callback = RefreshTimer;
    arguments.arg = this;
    arguments.dispatch_method = ESP_TIMER_TASK;
    arguments.name = "tab5_battery";
    arguments.skip_unhandled_events = true;
    if (esp_timer_create(&arguments, &refresh_timer_) != ESP_OK ||
        esp_timer_start_periodic(refresh_timer_, kRefreshIntervalUs) != ESP_OK) {
        ESP_LOGW(kTag, "periodic refresh unavailable");
    }
}

device::BatterySnapshot BatteryPeripheral::Snapshot() {
    if (executor_ == nullptr) {
        return {};
    }
    struct Request final {
        BatteryPeripheral* peripheral;
        device::BatterySnapshot snapshot;
    } request{this, last_snapshot_};
    const esp_err_t status = executor_->Invoke(
        buses::I2cExecutor::Priority::kLow,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.snapshot = requested.peripheral->RefreshOnWorker();
            return ESP_OK;
        },
        &request);
    return status == ESP_OK ? request.snapshot : last_snapshot_;
}

device::BatterySnapshot BatteryPeripheral::RefreshOnWorker() {
    const device::BatterySnapshot previous = last_snapshot_;
    drivers::Ina226Sample sample{};
    if (!monitor_.Read(sample, esp_timer_get_time())) {
        return last_snapshot_;
    }
    uint8_t charger_inputs = 0U;
    const bool charger_read =
        expander_ != nullptr && expander_->ReadInputPort(board::kChargerStatusExpanderUnit, charger_inputs) == ESP_OK;
    // The charger's status output is only ever evidence *for* external power:
    // it is asserted while a source charges the pack, and it pulses while no
    // pack is fitted (the state this board ships in), so it must never be read
    // as "no cable attached".
    const bool charger_active = charger_read && (charger_inputs & (1U << board::kChargerStatusExpanderBit)) != 0U;
    const bool battery_in_range =
        sample.bus_voltage_volts >= kMinimumBatteryVolts && sample.bus_voltage_volts <= kMaximumBatteryVolts;
    const bool charging = sample.shunt_current_amps >= kCurrentThresholdAmps;
    const bool discharging = sample.shunt_current_amps <= -kCurrentThresholdAmps;
    // Power source, in the order the evidence is trusted: a pack that reads out
    // of range means the board runs from USB; a charging pack or an active
    // charger means a source is attached; a discharge means battery-only
    // operation, the single state the idle power policy may act on. Everything
    // in between (idle or full pack on a charger) keeps the previous verdict, so
    // a charger negotiating around the threshold cannot restart the idle timer.
    if (!battery_in_range || charging || charger_active) {
        external_power_connected_ = true;
    } else if (discharging) {
        external_power_connected_ = false;
    }
    const uint8_t percent =
        battery_in_range ? ApplyHysteresis(previous.percent, PercentForVoltage(sample.bus_voltage_volts)) : 0U;
    last_snapshot_ = {
        .percent = percent,
        .available = battery_in_range,
        .charging = battery_in_range && charging,
        .discharging = battery_in_range && discharging,
        .charging_available = true,
        .external_power_connected = external_power_connected_,
        .external_power_available = true,
    };
    if (!sample_logged_ || previous.percent != last_snapshot_.percent ||
        previous.available != last_snapshot_.available || previous.charging != last_snapshot_.charging ||
        previous.discharging != last_snapshot_.discharging ||
        previous.external_power_connected != last_snapshot_.external_power_connected) {
        ESP_LOGD(kTag,
                 "sample: bus=%.2f V current=%.3f A power=%.2f W chg_stat=0x%02x(%s) -> soc=%u%% present=%s "
                 "charging=%s discharging=%s ext=%s",
                 static_cast<double>(sample.bus_voltage_volts), static_cast<double>(sample.shunt_current_amps),
                 static_cast<double>(sample.bus_power_watts), static_cast<unsigned>(charger_inputs),
                 charger_read ? "read" : "n/a", static_cast<unsigned>(last_snapshot_.percent),
                 last_snapshot_.available ? "yes" : "no", last_snapshot_.charging ? "yes" : "no",
                 last_snapshot_.discharging ? "yes" : "no", last_snapshot_.external_power_connected ? "yes" : "no");
        sample_logged_ = true;
    }
    NotifyIfChanged(previous, last_snapshot_);
    return last_snapshot_;
}

void BatteryPeripheral::RefreshTimer(void* context) {
    auto* battery = static_cast<BatteryPeripheral*>(context);
    if (battery == nullptr || battery->executor_ == nullptr ||
        battery->refresh_pending_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!battery->executor_->Post(buses::I2cExecutor::Priority::kLow, RefreshEntry, battery)) {
        battery->refresh_pending_.store(false, std::memory_order_release);
    }
}

esp_err_t BatteryPeripheral::RefreshEntry(void* context) {
    auto* battery = static_cast<BatteryPeripheral*>(context);
    if (battery == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    battery->refresh_pending_.store(false, std::memory_order_release);
    (void)battery->RefreshOnWorker();
    return ESP_OK;
}

void BatteryPeripheral::NotifyIfChanged(const device::BatterySnapshot& previous,
                                        const device::BatterySnapshot& current) {
    if (previous.percent == current.percent && previous.available == current.available &&
        previous.charging == current.charging && previous.discharging == current.discharging &&
        previous.external_power_connected == current.external_power_connected) {
        return;
    }
    device::BatteryStateChangeSink sink = state_change_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(state_change_context_.load(std::memory_order_acquire));
    }
}

void BatteryPeripheral::SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) {
    if (sink == nullptr) {
        state_change_sink_.store(nullptr, std::memory_order_release);
        state_change_context_.store(nullptr, std::memory_order_release);
        return;
    }
    state_change_context_.store(context, std::memory_order_release);
    state_change_sink_.store(sink, std::memory_order_release);
}

}  // namespace micropixel::platform::m5stack_tab5
