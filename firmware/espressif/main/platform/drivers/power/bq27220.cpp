#include "platform/drivers/power/bq27220.hpp"

#include <algorithm>

#include "esp_log.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "bq27220";
constexpr uint8_t kAddress = 0x55U;
constexpr uint8_t kVoltageRegister = 0x08U;
constexpr uint8_t kCurrentRegister = 0x0cU;
constexpr uint8_t kStateOfChargeRegister = 0x2cU;
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kI2cTimeoutMs = 50;
constexpr int64_t kProbeRetryUs = 10000000;
constexpr int64_t kReadFailureCooldownUs = 2000000;

}  // namespace

bool Bq27220::Read(Bq27220Sample& sample, int64_t now_us) {
    if (device_ == nullptr && now_us >= next_probe_us_ && !Attach(now_us)) {
        return false;
    }
    if (device_ == nullptr || now_us < read_cooldown_until_us_) {
        return false;
    }
    uint16_t raw_current = 0U;
    if (!ReadRegister(kVoltageRegister, sample.voltage_mv, now_us) ||
        !ReadRegister(kCurrentRegister, raw_current, now_us)) {
        return false;
    }
    sample.current_ma = static_cast<int16_t>(raw_current);
    return true;
}

bool Bq27220::ReadStateOfCharge(uint8_t& percent, int64_t now_us) {
    if (device_ == nullptr && now_us >= next_probe_us_ && !Attach(now_us)) {
        return false;
    }
    if (device_ == nullptr || now_us < read_cooldown_until_us_) {
        return false;
    }
    uint16_t raw_percent = 0U;
    if (!ReadRegister(kStateOfChargeRegister, raw_percent, now_us)) {
        return false;
    }
    percent = static_cast<uint8_t>(std::min<uint16_t>(raw_percent, 100U));
    return true;
}

bool Bq27220::Attach(int64_t now_us) {
    if (device_ != nullptr) {
        return true;
    }
    if (bus_ == nullptr) {
        return false;
    }
    const esp_err_t probe_status = i2c_master_probe(bus_, kAddress, kI2cTimeoutMs);
    if (probe_status != ESP_OK) {
        next_probe_us_ = now_us + kProbeRetryUs;
        if (!probe_warning_logged_) {
            ESP_LOGW(kTag, "unavailable at I2C address 0x%02x: %s", kAddress, esp_err_to_name(probe_status));
            probe_warning_logged_ = true;
        }
        return false;
    }

    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = kI2cSpeedHz;
    const esp_err_t status = i2c_master_bus_add_device(bus_, &config, &device_);
    if (status != ESP_OK) {
        device_ = nullptr;
        next_probe_us_ = now_us + kProbeRetryUs;
        ESP_LOGW(kTag, "could not attach: %s", esp_err_to_name(status));
        return false;
    }
    next_probe_us_ = 0;
    ESP_LOGI(kTag, "ready at I2C address 0x%02x", kAddress);
    return true;
}

bool Bq27220::ReadRegister(uint8_t address, uint16_t& value, int64_t now_us) {
    uint8_t bytes[2]{};
    const esp_err_t status =
        i2c_master_transmit_receive(device_, &address, sizeof(address), bytes, sizeof(bytes), kI2cTimeoutMs);
    if (status != ESP_OK) {
        ++consecutive_read_failures_;
        if (consecutive_read_failures_ == 1U || consecutive_read_failures_ % 10U == 0U) {
            ESP_LOGW(kTag, "register 0x%02x read failed: %s (consecutive=%lu)", address, esp_err_to_name(status),
                     static_cast<unsigned long>(consecutive_read_failures_));
        }
        if (consecutive_read_failures_ >= 2U) {
            read_cooldown_until_us_ = now_us + kReadFailureCooldownUs;
        }
        return false;
    }
    consecutive_read_failures_ = 0U;
    read_cooldown_until_us_ = 0;
    value = static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
    return true;
}

}  // namespace micropixel::platform::drivers
