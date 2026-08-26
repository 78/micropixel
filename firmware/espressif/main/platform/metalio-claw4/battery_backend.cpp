#include "platform/metalio-claw4/battery_backend.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_battery";
constexpr uint8_t kBq27220Address = 0x55U;
constexpr uint8_t kVoltageRegister = 0x08U;
constexpr uint8_t kCurrentRegister = 0x0cU;
constexpr uint8_t kTcaInputPort1 = 0x01U;
constexpr uint8_t kTcaConfigurationPort1 = 0x07U;
constexpr uint8_t kUsbInsertMask = 1U << 2U;       // P1-2, active-high VBUS divider.
constexpr uint8_t kWirelessChargeMask = 1U << 3U;  // P1-3, active-high WX_OUT divider.
constexpr uint8_t kExternalPowerInputMask = kUsbInsertMask | kWirelessChargeMask;
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kI2cTimeoutMs = 50;
constexpr int64_t kProbeRetryUs = 10000000;
constexpr int64_t kReadFailureCooldownUs = 2000000;
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

void BatteryBackend::Initialize(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t io_expander) {
    bus_ = bus;
    io_expander_ = io_expander;
    if (!ConfigurePowerDetectionInputs()) {
        ESP_LOGW(kTag, "external-power inputs could not be configured");
    }
    (void)Attach();
}

device::BatterySnapshot BatteryBackend::Snapshot() {
    bool external_power_connected = false;
    if (ReadExternalPower(external_power_connected)) {
        external_power_connected_ = external_power_connected;
        external_power_available_ = true;
    }

    const int64_t now_us = esp_timer_get_time();
    if (device_ == nullptr && now_us >= next_probe_us_) {
        (void)Attach();
    }
    if (device_ == nullptr || now_us < read_cooldown_until_us_) {
        return {.percent = last_percent_,
                .available = sample_available_,
                .charging = charging_,
                .discharging = discharging_,
                .charging_available = charging_available_,
                .external_power_connected = external_power_connected_,
                .external_power_available = external_power_available_};
    }

    uint16_t voltage_mv = 0U;
    if (!ReadRegister(kVoltageRegister, voltage_mv)) {
        return {.percent = last_percent_,
                .available = sample_available_,
                .charging = charging_,
                .discharging = discharging_,
                .charging_available = charging_available_,
                .external_power_connected = external_power_connected_,
                .external_power_available = external_power_available_};
    }
    last_percent_ = Filter(EstimatePercent(voltage_mv), now_us);
    sample_available_ = true;

    uint16_t raw_current_ma = 0U;
    if (ReadRegister(kCurrentRegister, raw_current_ma)) {
        const int16_t current_ma = static_cast<int16_t>(raw_current_ma);
        const bool charging = current_ma > kChargingThresholdMa;
        const bool discharging = current_ma < -kChargingThresholdMa;
        if (!charging_available_ || charging != charging_ || discharging != discharging_) {
            const char* direction = charging ? "charging" : (discharging ? "discharging" : "idle");
            ESP_LOGI(kTag, "battery %s: current=%d mA", direction, static_cast<int>(current_ma));
        }
        charging_ = charging;
        discharging_ = discharging;
        charging_available_ = true;
    }
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

bool BatteryBackend::Attach() {
    if (device_ != nullptr) {
        return true;
    }
    if (bus_ == nullptr) {
        return false;
    }

    const esp_err_t probe_status = i2c_master_probe(bus_, kBq27220Address, kI2cTimeoutMs);
    if (probe_status != ESP_OK) {
        next_probe_us_ = esp_timer_get_time() + kProbeRetryUs;
        if (!probe_warning_logged_) {
            ESP_LOGW(kTag, "BQ27220 unavailable at I2C address 0x%02x: %s", kBq27220Address,
                     esp_err_to_name(probe_status));
            probe_warning_logged_ = true;
        }
        return false;
    }

    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kBq27220Address;
    config.scl_speed_hz = kI2cSpeedHz;
    const esp_err_t add_status = i2c_master_bus_add_device(bus_, &config, &device_);
    if (add_status != ESP_OK) {
        device_ = nullptr;
        next_probe_us_ = esp_timer_get_time() + kProbeRetryUs;
        ESP_LOGW(kTag, "could not attach BQ27220: %s", esp_err_to_name(add_status));
        return false;
    }

    next_probe_us_ = 0;
    ESP_LOGI(kTag, "BQ27220 ready at I2C address 0x%02x", kBq27220Address);
    return true;
}

bool BatteryBackend::ReadRegister(uint8_t address, uint16_t& value) {
    uint8_t bytes[2]{};
    const esp_err_t status =
        i2c_master_transmit_receive(device_, &address, sizeof(address), bytes, sizeof(bytes), kI2cTimeoutMs);
    if (status != ESP_OK) {
        ++consecutive_read_failures_;
        if (consecutive_read_failures_ == 1U || consecutive_read_failures_ % 10U == 0U) {
            ESP_LOGW(kTag, "BQ27220 register 0x%02x read failed: %s (consecutive=%lu)", address,
                     esp_err_to_name(status), static_cast<unsigned long>(consecutive_read_failures_));
        }
        if (consecutive_read_failures_ >= 2U) {
            read_cooldown_until_us_ = esp_timer_get_time() + kReadFailureCooldownUs;
        }
        return false;
    }

    consecutive_read_failures_ = 0U;
    read_cooldown_until_us_ = 0;
    value = static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
    return true;
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
