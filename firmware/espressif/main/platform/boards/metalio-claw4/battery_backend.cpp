#include "platform/boards/metalio-claw4/battery_backend.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_battery";
constexpr uint8_t kTcaInputPort1 = 0x01U;
constexpr uint8_t kTcaConfigurationPort1 = 0x07U;
constexpr uint8_t kUsbInsertMask = 1U << 2U;       // P1-2, active-high VBUS divider.
constexpr uint8_t kWirelessChargeMask = 1U << 3U;  // P1-3, active-high WX_OUT divider.
constexpr uint8_t kExternalPowerInputMask = kUsbInsertMask | kWirelessChargeMask;
constexpr int kI2cTimeoutMs = 50;
constexpr uint16_t kEmptyVoltageMv = 3300U;
constexpr uint16_t kFullVoltageMv = 4200U;
constexpr int16_t kChargingThresholdMa = 5;

constexpr bool ExternalPowerConnected(uint8_t port1) {
    const bool usb_connected = (port1 & kUsbInsertMask) != 0U;
    const bool wireless_connected = (port1 & kWirelessChargeMask) != 0U;
    return usb_connected || wireless_connected;
}

static_assert(ExternalPowerConnected(kExternalPowerInputMask));
static_assert(ExternalPowerConnected(kWirelessChargeMask));
static_assert(ExternalPowerConnected(kUsbInsertMask));
static_assert(!ExternalPowerConnected(0U));

uint8_t EstimatePercent(uint16_t voltage_mv) {
    if (voltage_mv <= kEmptyVoltageMv) {
        return 0U;
    }
    if (voltage_mv >= kFullVoltageMv) {
        return 100U;
    }
    constexpr uint32_t kVoltageRangeMv = kFullVoltageMv - kEmptyVoltageMv;
    const uint32_t above_empty_mv = voltage_mv - kEmptyVoltageMv;
    return static_cast<uint8_t>((above_empty_mv * 100U + kVoltageRangeMv / 2U) / kVoltageRangeMv);
}

}  // namespace

void BatteryBackend::Initialize(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t io_expander,
                                common::I2cExecutor& i2c_executor) {
    fuel_gauge_.Bind(bus);
    io_expander_ = io_expander;
    i2c_executor_ = &i2c_executor;
    const esp_err_t status = i2c_executor.Invoke(
        common::I2cExecutor::Priority::kLow,
        [](void* context) {
            static_cast<BatteryBackend*>(context)->InitializeOnWorker();
            return ESP_OK;
        },
        this);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "battery initialization could not run on the shared I2C executor: %s", esp_err_to_name(status));
    }
}

void BatteryBackend::InitializeOnWorker() {
    if (!ConfigurePowerDetectionInputs()) {
        ESP_LOGW(kTag, "external-power inputs could not be configured");
    }
    drivers::Bq27220Sample sample{};
    (void)fuel_gauge_.Read(sample, esp_timer_get_time());
}

device::BatterySnapshot BatteryBackend::Snapshot() {
    if (i2c_executor_ == nullptr) {
        return {};
    }
    struct Request final {
        BatteryBackend* backend;
        device::BatterySnapshot snapshot;
    } request{this, {}};
    const esp_err_t status = i2c_executor_->Invoke(
        common::I2cExecutor::Priority::kLow,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            requested.snapshot = requested.backend->RefreshOnWorker();
            return ESP_OK;
        },
        &request);
    return status == ESP_OK ? request.snapshot : device::BatterySnapshot{};
}

device::BatterySnapshot BatteryBackend::RefreshOnWorker() {
    bool external_power_connected = false;
    if (ReadExternalPower(external_power_connected)) {
        external_power_connected_ = external_power_connected;
        external_power_available_ = true;
    }

    const int64_t now_us = esp_timer_get_time();
    drivers::Bq27220Sample fuel_sample{};
    if (!fuel_gauge_.Read(fuel_sample, now_us)) {
        return {.percent = last_percent_,
                .available = sample_available_,
                .charging = charging_,
                .discharging = discharging_,
                .charging_available = charging_available_,
                .external_power_connected = external_power_connected_,
                .external_power_available = external_power_available_};
    }

    last_percent_ = Filter(EstimatePercent(fuel_sample.voltage_mv), now_us);
    sample_available_ = true;

    const bool charging = fuel_sample.current_ma > kChargingThresholdMa;
    const bool discharging = fuel_sample.current_ma < -kChargingThresholdMa;
    if (!charging_available_ || charging != charging_ || discharging != discharging_) {
        const char* direction = charging ? "charging" : (discharging ? "discharging" : "idle");
        ESP_LOGI(kTag, "battery %s: current=%d mA", direction, static_cast<int>(fuel_sample.current_ma));
    }
    charging_ = charging;
    discharging_ = discharging;
    charging_available_ = true;
    return {.percent = last_percent_,
            .available = true,
            .charging = charging_,
            .discharging = discharging_,
            .charging_available = charging_available_,
            .external_power_connected = external_power_connected_,
            .external_power_available = external_power_available_};
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

void BatteryBackend::NotifyExternalPowerChanged() {
    device::BatteryStateChangeSink sink = state_change_sink_.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(state_change_context_.load(std::memory_order_acquire));
    }
}

bool BatteryBackend::ConfigurePowerDetectionInputs() {
    if (io_expander_ == nullptr) {
        return false;
    }
    uint8_t configuration = 0U;
    esp_err_t status =
        i2c_master_transmit_receive(io_expander_, &kTcaConfigurationPort1, sizeof(kTcaConfigurationPort1),
                                    &configuration, sizeof(configuration), kI2cTimeoutMs);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "TCA9555 configuration read failed: %s", esp_err_to_name(status));
        return false;
    }
    if ((configuration & kExternalPowerInputMask) == kExternalPowerInputMask) {
        return true;
    }
    const uint8_t transaction[] = {kTcaConfigurationPort1,
                                   static_cast<uint8_t>(configuration | kExternalPowerInputMask)};
    status = i2c_master_transmit(io_expander_, transaction, sizeof(transaction), kI2cTimeoutMs);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "TCA9555 power-input configuration failed: %s", esp_err_to_name(status));
        return false;
    }
    return true;
}

bool BatteryBackend::ReadExternalPower(bool& connected) {
    if (io_expander_ == nullptr) {
        return false;
    }
    uint8_t port1 = 0U;
    const esp_err_t status = i2c_master_transmit_receive(io_expander_, &kTcaInputPort1, sizeof(kTcaInputPort1), &port1,
                                                         sizeof(port1), kI2cTimeoutMs);
    if (status != ESP_OK) {
        ++consecutive_power_read_failures_;
        if (consecutive_power_read_failures_ == 1U || consecutive_power_read_failures_ % 10U == 0U) {
            ESP_LOGW(kTag, "TCA9555 external-power read failed: %s (consecutive=%lu)", esp_err_to_name(status),
                     static_cast<unsigned long>(consecutive_power_read_failures_));
        }
        return false;
    }
    if (consecutive_power_read_failures_ != 0U) {
        ESP_LOGI(kTag, "TCA9555 external-power reads recovered after %lu failures",
                 static_cast<unsigned long>(consecutive_power_read_failures_));
        consecutive_power_read_failures_ = 0U;
    }

    const bool usb_connected = (port1 & kUsbInsertMask) != 0U;
    const bool wireless_connected = (port1 & kWirelessChargeMask) != 0U;
    connected = ExternalPowerConnected(port1);
    if (!external_power_available_ || connected != external_power_connected_) {
        ESP_LOGI(kTag, "external power %s: USB=%s wireless=%s", connected ? "connected" : "disconnected",
                 usb_connected ? "yes" : "no", wireless_connected ? "yes" : "no");
    }
    return true;
}

uint8_t BatteryBackend::Filter(uint8_t sample, int64_t now_us) {
    if (!filter_primed_) {
        filter_.fill(sample);
        filter_sum_ = static_cast<uint32_t>(sample) * kFilterCapacity;
        filter_index_ = 0U;
        filter_last_update_us_ = now_us;
        filter_primed_ = true;
        return sample;
    }

    const int64_t elapsed_us = now_us - filter_last_update_us_;
    uint32_t elapsed_steps = 1U;
    if (elapsed_us > kFilterStepUs) {
        elapsed_steps = static_cast<uint32_t>(elapsed_us / kFilterStepUs);
        if (elapsed_steps > kFilterCapacity) {
            elapsed_steps = kFilterCapacity;
        }
    }
    filter_last_update_us_ = now_us;

    for (uint32_t step = 0U; step < elapsed_steps; ++step) {
        filter_sum_ -= filter_[filter_index_];
        filter_[filter_index_] = sample;
        filter_sum_ += sample;
        filter_index_ = (filter_index_ + 1U) % kFilterCapacity;
    }
    return static_cast<uint8_t>((filter_sum_ + kFilterCapacity / 2U) / kFilterCapacity);
}

}  // namespace micropixel::platform::metalio_claw4
