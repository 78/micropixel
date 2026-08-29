#include "platform/boards/esp-mosaico/battery_peripheral.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "platform/boards/esp-mosaico/battery_power_policy.hpp"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_battery";
constexpr uint64_t kRefreshIntervalUs = 2U * 1000U * 1000U;

}  // namespace

BatteryPeripheral::~BatteryPeripheral() {
    if (refresh_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(refresh_timer_, portMAX_DELAY);
        (void)esp_timer_delete(refresh_timer_);
    }
}

void BatteryPeripheral::Initialize(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    fuel_gauge_.Bind(bus);
    executor_ = &executor;
    const esp_err_t status = executor.Invoke(
        buses::I2cExecutor::Priority::kLow,
        [](void* context) {
            (void)static_cast<BatteryPeripheral*>(context)->RefreshOnWorker();
            return ESP_OK;
        },
        this);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "initial probe could not run on the shared I2C executor: %s", esp_err_to_name(status));
    }
    esp_timer_create_args_t timer_config{};
    timer_config.callback = RefreshTimer;
    timer_config.arg = this;
    timer_config.dispatch_method = ESP_TIMER_TASK;
    timer_config.name = "mosaico_battery";
    timer_config.skip_unhandled_events = true;
    if (esp_timer_create(&timer_config, &refresh_timer_) != ESP_OK ||
        esp_timer_start_periodic(refresh_timer_, kRefreshIntervalUs) != ESP_OK) {
        ESP_LOGW(kTag, "periodic battery refresh unavailable");
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
    drivers::Bq27220Sample sample{};
    const int64_t now_us = esp_timer_get_time();
    uint8_t percent = 0U;
    if (!fuel_gauge_.Read(sample, now_us) || !fuel_gauge_.ReadStateOfCharge(percent, now_us)) {
        return last_snapshot_;
    }
    const bool charging = battery_policy::IsCharging(sample.current_ma);
    const bool discharging = battery_policy::IsDischarging(sample.current_ma);
    const bool external_power_connected = battery_policy::ExternalPowerConnected(sample.current_ma);
    last_snapshot_ = {
        .percent = percent,
        .available = true,
        .charging = charging,
        .discharging = discharging,
        .charging_available = true,
        .external_power_connected = external_power_connected,
        .external_power_available = true,
    };
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
        previous.charging_available == current.charging_available &&
        previous.external_power_connected == current.external_power_connected &&
        previous.external_power_available == current.external_power_available) {
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

}  // namespace micropixel::platform::esp_mosaico
