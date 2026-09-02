#include "platform/boards/m5stack-cores3/battery_peripheral.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "platform/boards/m5stack-cores3/board_hardware.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::m5stack_cores3 {
namespace {

constexpr char kTag[] = "cores3_battery";
constexpr uint64_t kRefreshIntervalUs = 2U * 1000U * 1000U;

}  // namespace

BatteryPeripheral::~BatteryPeripheral() {
    if (refresh_timer_ != nullptr) {
        (void)esp_timer_stop_blocking(refresh_timer_, portMAX_DELAY);
        (void)esp_timer_delete(refresh_timer_);
    }
}

void BatteryPeripheral::Initialize(BoardHardware& hardware, buses::I2cExecutor& executor) {
    hardware_ = &hardware;
    executor_ = &executor;
    const esp_err_t status = executor.Invoke(buses::I2cExecutor::Priority::kLow, RefreshEntry, this);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "initial refresh failed: %s", esp_err_to_name(status));
    }
    esp_timer_create_args_t arguments{};
    arguments.callback = RefreshTimer;
    arguments.arg = this;
    arguments.dispatch_method = ESP_TIMER_TASK;
    arguments.name = "cores3_battery";
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
    drivers::Axp2101BatterySample sample{};
    if (hardware_ == nullptr || hardware_->ReadBatteryOnBus(sample) != ESP_OK) {
        return last_snapshot_;
    }
    last_snapshot_ = {
        .percent = sample.percent,
        .available = sample.battery_present,
        .charging = sample.charging,
        .discharging = sample.discharging,
        .charging_available = true,
        .external_power_connected = sample.external_power_connected,
        .external_power_available = true,
    };
    if (!sample_logged_ || previous.available != last_snapshot_.available ||
        previous.percent != last_snapshot_.percent || previous.charging != last_snapshot_.charging ||
        previous.discharging != last_snapshot_.discharging ||
        previous.external_power_connected != last_snapshot_.external_power_connected) {
        ESP_LOGI(kTag, "sample: soc=%u%% present=%s charging=%s external=%s",
                 static_cast<unsigned>(last_snapshot_.percent), last_snapshot_.available ? "yes" : "no",
                 last_snapshot_.charging ? "yes" : "no", last_snapshot_.external_power_connected ? "yes" : "no");
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

}  // namespace micropixel::platform::m5stack_cores3
