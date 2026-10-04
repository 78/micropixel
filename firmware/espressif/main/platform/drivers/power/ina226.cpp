// SPDX-License-Identifier: Apache-2.0
#include "platform/drivers/power/ina226.hpp"

#include <cmath>

#include "esp_log.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "ina226";
constexpr uint32_t kI2cSpeedHz = 400'000U;
constexpr int kTimeoutMs = 50;
constexpr int64_t kProbeRetryUs = 1000'000;
constexpr int64_t kReadCooldownUs = 1000'000;

// Registers.
constexpr uint8_t kRegisterConfiguration = 0x00U;
constexpr uint8_t kRegisterBusVoltage = 0x02U;
constexpr uint8_t kRegisterPower = 0x03U;
constexpr uint8_t kRegisterCurrent = 0x04U;
constexpr uint8_t kRegisterCalibration = 0x05U;

// Averaging 16, 1.1 ms bus and shunt conversion, continuous shunt+bus mode.
constexpr uint16_t kConfigurationValue = 0x2938U;
// Content of the calibration register is 0.00512 / (current_lsb * shunt).
constexpr float kCalibrationNumerator = 0.00512F;

// INA226 bus-voltage LSB and the power LSB relative to the current LSB.
constexpr float kBusVoltageLsbVolts = 0.00125F;
constexpr float kPowerLsbFactor = 25.0F;

constexpr float kMinimumShuntOhms = 0.0001F;

}  // namespace

Ina226::~Ina226() {
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
    }
}

esp_err_t Ina226::Bind(i2c_master_bus_handle_t bus, Ina226Config config) {
    if (bus == nullptr || config.shunt_ohms < kMinimumShuntOhms || config.calibration_max_current_amps <= 0.0F) {
        return ESP_ERR_INVALID_ARG;
    }
    bus_ = bus;
    config_ = config;
    current_lsb_amps_ = config.calibration_max_current_amps / 32768.0F;
    return ESP_OK;
}

bool Ina226::WriteRegister(uint8_t address, uint16_t value) {
    const uint8_t payload[3] = {address, static_cast<uint8_t>(value >> 8U), static_cast<uint8_t>(value & 0xFFU)};
    return i2c_master_transmit(device_, payload, sizeof(payload), kTimeoutMs) == ESP_OK;
}

bool Ina226::ReadRegister(uint8_t address, uint16_t& value) {
    uint8_t payload[2] = {};
    if (i2c_master_transmit_receive(device_, &address, sizeof(address), payload, sizeof(payload), kTimeoutMs) !=
        ESP_OK) {
        return false;
    }
    value = static_cast<uint16_t>((static_cast<uint16_t>(payload[0]) << 8U) | payload[1]);
    return true;
}

void Ina226::DropDevice(const char* operation, esp_err_t status, int64_t now_us) {
    ESP_LOGW(kTag, "%s failed: %s", operation, esp_err_to_name(status));
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    next_probe_us_ = now_us + kProbeRetryUs;
}

bool Ina226::Prepare(int64_t now_us) {
    if (device_ != nullptr) {
        return true;
    }
    if (bus_ == nullptr || now_us < next_probe_us_) {
        return false;
    }
    if (i2c_master_probe(bus_, config_.address, kTimeoutMs) != ESP_OK) {
        next_probe_us_ = now_us + kProbeRetryUs;
        if (!probe_failed_logged_) {
            ESP_LOGW(kTag, "no INA226 at 0x%02x", config_.address);
            probe_failed_logged_ = true;
        }
        return false;
    }
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = config_.address;
    device_config.scl_speed_hz = kI2cSpeedHz;
    if (i2c_master_bus_add_device(bus_, &device_config, &device_) != ESP_OK) {
        device_ = nullptr;
        next_probe_us_ = now_us + kProbeRetryUs;
        return false;
    }
    if (!WriteRegister(kRegisterConfiguration, kConfigurationValue)) {
        DropDevice("configuration write", ESP_FAIL, now_us);
        return false;
    }
    const auto calibration =
        static_cast<uint16_t>(std::lround(kCalibrationNumerator / (current_lsb_amps_ * config_.shunt_ohms)));
    if (!WriteRegister(kRegisterCalibration, calibration)) {
        DropDevice("calibration write", ESP_FAIL, now_us);
        return false;
    }
    ESP_LOGI(kTag, "ready: address=0x%02x shunt=%.4f ohm current-lsb=%.6f A calibration=%u", config_.address,
             static_cast<double>(config_.shunt_ohms), static_cast<double>(current_lsb_amps_),
             static_cast<unsigned>(calibration));
    return true;
}

bool Ina226::Read(Ina226Sample& sample, int64_t now_us) {
    if (!Prepare(now_us)) {
        return false;
    }
    if (now_us < read_cooldown_until_us_) {
        return false;
    }
    uint16_t bus_raw = 0U;
    uint16_t current_raw = 0U;
    uint16_t power_raw = 0U;
    if (!ReadRegister(kRegisterBusVoltage, bus_raw) || !ReadRegister(kRegisterCurrent, current_raw) ||
        !ReadRegister(kRegisterPower, power_raw)) {
        DropDevice("register read", ESP_FAIL, now_us);
        read_cooldown_until_us_ = now_us + kReadCooldownUs;
        return false;
    }
    sample.bus_voltage_volts = static_cast<float>(bus_raw) * kBusVoltageLsbVolts;
    sample.shunt_current_amps = static_cast<float>(static_cast<int16_t>(current_raw)) * current_lsb_amps_;
    sample.bus_power_watts = static_cast<float>(power_raw) * kPowerLsbFactor * current_lsb_amps_;
    return true;
}

}  // namespace micropixel::platform::drivers
