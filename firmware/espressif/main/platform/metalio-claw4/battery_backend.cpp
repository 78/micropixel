#include "platform/metalio-claw4/battery_backend.hpp"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_battery";
constexpr uint8_t kBq27220Address = 0x55U;
constexpr uint8_t kVoltageRegister = 0x08U;
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kI2cTimeoutMs = 50;
constexpr int64_t kProbeRetryUs = 10000000;
constexpr int64_t kReadFailureCooldownUs = 2000000;
constexpr uint16_t kEmptyVoltageMv = 3300U;
constexpr uint16_t kFullVoltageMv = 4200U;

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

void BatteryBackend::Initialize(i2c_master_bus_handle_t bus) {
    bus_ = bus;
    (void)Attach();
}

device::BatterySnapshot BatteryBackend::Snapshot() {
    const int64_t now_us = esp_timer_get_time();
    if (device_ == nullptr && now_us >= next_probe_us_) {
        (void)Attach();
    }
    if (device_ == nullptr || now_us < read_cooldown_until_us_) {
        return {.percent = last_percent_, .available = sample_available_};
    }

    uint16_t voltage_mv = 0U;
    if (!ReadRegister(kVoltageRegister, voltage_mv)) {
        return {.percent = last_percent_, .available = sample_available_};
    }
    last_percent_ = Filter(EstimatePercent(voltage_mv));
    sample_available_ = true;
    return {.percent = last_percent_, .available = true};
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

uint8_t BatteryBackend::Filter(uint8_t sample) {
    if (!filter_primed_) {
        filter_.fill(sample);
        filter_sum_ = static_cast<uint32_t>(sample) * kFilterCapacity;
        filter_index_ = 0U;
        filter_primed_ = true;
        return sample;
    }

    filter_sum_ -= filter_[filter_index_];
    filter_[filter_index_] = sample;
    filter_sum_ += sample;
    filter_index_ = (filter_index_ + 1U) % kFilterCapacity;
    return static_cast<uint8_t>((filter_sum_ + kFilterCapacity / 2U) / kFilterCapacity);
}

}  // namespace micropixel::platform::metalio_claw4
