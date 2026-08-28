#include "platform/boards/esp-mosaico/battery_backend.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::esp_mosaico {
namespace {

constexpr char kTag[] = "mosaico_battery";
constexpr int16_t kChargingThresholdMa = 5;

}  // namespace

void BatteryBackend::Initialize(i2c_master_bus_handle_t bus, common::I2cExecutor& executor) {
    fuel_gauge_.Bind(bus);
    executor_ = &executor;
    const esp_err_t status = executor.Invoke(
        common::I2cExecutor::Priority::kLow,
        [](void* context) {
            (void)static_cast<BatteryBackend*>(context)->RefreshOnWorker();
            return ESP_OK;
        },
        this);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "initial probe could not run on the shared I2C executor: %s", esp_err_to_name(status));
    }
}

device::BatterySnapshot BatteryBackend::Snapshot() {
    if (executor_ == nullptr) {
        return {};
    }
    struct Request final {
        BatteryBackend* backend;
        device::BatterySnapshot snapshot;
    } request{this, last_snapshot_};
    const esp_err_t status = executor_->Invoke(
        common::I2cExecutor::Priority::kLow,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.snapshot = requested.backend->RefreshOnWorker();
            return ESP_OK;
        },
        &request);
    return status == ESP_OK ? request.snapshot : last_snapshot_;
}

device::BatterySnapshot BatteryBackend::RefreshOnWorker() {
    drivers::Bq27220Sample sample{};
    const int64_t now_us = esp_timer_get_time();
    uint8_t percent = 0U;
    if (!fuel_gauge_.Read(sample, now_us) || !fuel_gauge_.ReadStateOfCharge(percent, now_us)) {
        return last_snapshot_;
    }
    const bool charging = sample.current_ma > kChargingThresholdMa;
    const bool discharging = sample.current_ma < -kChargingThresholdMa;
    last_snapshot_ = {
        .percent = percent,
        .available = true,
        .charging = charging,
        .discharging = discharging,
        .charging_available = true,
    };
    return last_snapshot_;
}

void BatteryBackend::SetStateChangeSink(device::BatteryStateChangeSink sink, void* context) {
    if (sink == nullptr) {
        state_change_sink_.store(nullptr, std::memory_order_release);
        state_change_context_.store(nullptr, std::memory_order_release);
        return;
    }
    state_change_context_.store(context, std::memory_order_release);
    state_change_sink_.store(sink, std::memory_order_release);
}

}  // namespace micropixel::platform::esp_mosaico
