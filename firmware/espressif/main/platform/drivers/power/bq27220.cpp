#include "platform/drivers/power/bq27220.hpp"

#include <algorithm>
#include <array>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "bq27220";
constexpr uint8_t kAddress = 0x55U;
constexpr uint16_t kDeviceNumber = 0x0220U;
constexpr uint8_t kControlRegister = 0x00U;
constexpr uint8_t kVoltageRegister = 0x08U;
constexpr uint8_t kBatteryStatusRegister = 0x0aU;
constexpr uint8_t kCurrentRegister = 0x0cU;
constexpr uint8_t kStateOfChargeRegister = 0x2cU;
constexpr uint8_t kOperationStatusRegister = 0x3aU;
constexpr uint8_t kDesignCapacityRegister = 0x3cU;
constexpr uint8_t kDataMemoryAddressRegister = 0x3eU;
constexpr uint8_t kMacDataRegister = 0x40U;
constexpr uint8_t kMacChecksumRegister = 0x60U;
constexpr uint16_t kDeviceNumberCommand = 0x0001U;
constexpr uint16_t kSealCommand = 0x0030U;
constexpr uint16_t kEnterConfigurationCommand = 0x0090U;
constexpr uint16_t kExitConfigurationReinitializeCommand = 0x0091U;
constexpr uint16_t kUnsealKey1 = 0x0414U;
constexpr uint16_t kUnsealKey2 = 0x3672U;
constexpr uint16_t kFullChargedMask = 1U << 9U;
constexpr uint16_t kConfigurationModeMask = 1U << 10U;
constexpr uint16_t kSecurityMask = 0x0006U;
constexpr uint16_t kSecuritySealed = 0x0006U;
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kI2cTimeoutMs = 50;
constexpr int64_t kProbeRetryUs = 10000000;
constexpr int64_t kReadFailureCooldownUs = 2000000;
constexpr int64_t kProfileRetryUs = 10000000;
constexpr uint32_t kConfigurationPollAttempts = 20U;

void DelayMs(uint32_t milliseconds) { vTaskDelay(pdMS_TO_TICKS(milliseconds)); }

}  // namespace

bool Bq27220::Read(Bq27220Sample& sample, int64_t now_us) {
    if (!Prepare(now_us)) {
        return false;
    }
    uint16_t raw_current = 0U;
    if (!ReadRegister(kVoltageRegister, sample.voltage_mv, now_us) ||
        !ReadRegister(kCurrentRegister, raw_current, now_us) ||
        !ReadRegister(kBatteryStatusRegister, sample.battery_status, now_us)) {
        return false;
    }
    sample.current_ma = static_cast<int16_t>(raw_current);
    sample.full_charged = (sample.battery_status & kFullChargedMask) != 0U;
    return true;
}

bool Bq27220::ReadStateOfCharge(uint8_t& percent, int64_t now_us) {
    if (!Prepare(now_us)) {
        return false;
    }
    uint16_t raw_percent = 0U;
    if (!ReadRegister(kStateOfChargeRegister, raw_percent, now_us)) {
        return false;
    }
    percent = static_cast<uint8_t>(std::min<uint16_t>(raw_percent, 100U));
    return true;
}

bool Bq27220::Prepare(int64_t now_us) {
    if (device_ == nullptr && (now_us < next_probe_us_ || !Attach(now_us))) {
        return false;
    }
    if (device_ == nullptr || now_us < read_cooldown_until_us_) {
        return false;
    }
    if (profile_ == nullptr || profile_configured_) {
        return true;
    }
    if (now_us < next_profile_retry_us_) {
        return false;
    }
    profile_configured_ = ConfigureProfile(now_us);
    if (!profile_configured_) {
        next_profile_retry_us_ = now_us + kProfileRetryUs;
        ESP_LOGW(kTag, "battery profile configuration failed; retrying later");
    }
    return profile_configured_;
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

bool Bq27220::ConfigureProfile(int64_t now_us) {
    if (profile_ == nullptr) {
        return true;
    }

    bool configuration_mode = false;
    bool success = false;
    do {
        if (!Control(kDeviceNumberCommand, now_us)) {
            break;
        }
        DelayMs(15U);
        uint16_t device_number = 0U;
        if (!ReadRegister(kMacDataRegister, device_number, now_us) || device_number != kDeviceNumber) {
            ESP_LOGE(kTag, "unexpected device number 0x%04x", static_cast<unsigned>(device_number));
            break;
        }
        if (!Unseal(now_us)) {
            break;
        }
        if (ProfileMatches(now_us)) {
            ESP_LOGI(kTag, "battery profile already active: %u mAh",
                     static_cast<unsigned>(profile_->design_capacity_mah));
            success = true;
            break;
        }

        ESP_LOGW(kTag, "updating battery profile: %u mAh", static_cast<unsigned>(profile_->design_capacity_mah));
        if (!Control(kEnterConfigurationCommand, now_us) || !WaitForConfigurationMode(true, now_us)) {
            break;
        }
        configuration_mode = true;
        bool profile_written = true;
        for (const Bq27220DataMemoryValue& parameter : profile_->parameters) {
            if (!WriteDataMemory(parameter, now_us)) {
                profile_written = false;
                break;
            }
        }
        if (!profile_written || !Control(kExitConfigurationReinitializeCommand, now_us)) {
            break;
        }
        configuration_mode = false;
        if (!WaitForConfigurationMode(false, now_us)) {
            break;
        }
        DelayMs(10U);
        if (!ProfileMatches(now_us)) {
            ESP_LOGE(kTag, "battery profile verification failed");
            break;
        }
        ESP_LOGI(kTag, "battery profile update complete");
        success = true;
    } while (false);

    if (configuration_mode) {
        (void)Control(kExitConfigurationReinitializeCommand, now_us);
        (void)WaitForConfigurationMode(false, now_us);
    }
    if (!Seal(now_us)) {
        ESP_LOGE(kTag, "could not seal BQ27220 after profile access");
        success = false;
    }
    return success;
}

bool Bq27220::ProfileMatches(int64_t now_us) {
    if (profile_ == nullptr) {
        return true;
    }
    uint16_t design_capacity = 0U;
    if (!ReadRegister(kDesignCapacityRegister, design_capacity, now_us) ||
        design_capacity != profile_->design_capacity_mah) {
        return false;
    }
    for (const Bq27220DataMemoryValue& parameter : profile_->verification_parameters) {
        uint16_t value = 0U;
        if (!ReadDataMemory(parameter, value, now_us) || value != parameter.value) {
            return false;
        }
    }
    return true;
}

bool Bq27220::Unseal(int64_t now_us) {
    uint16_t operation_status = 0U;
    if (!ReadOperationStatus(operation_status, now_us)) {
        return false;
    }
    if ((operation_status & kSecurityMask) != kSecuritySealed) {
        return true;
    }
    if (!Control(kUnsealKey1, now_us)) {
        return false;
    }
    DelayMs(10U);
    if (!Control(kUnsealKey2, now_us)) {
        return false;
    }
    DelayMs(10U);
    return ReadOperationStatus(operation_status, now_us) && (operation_status & kSecurityMask) != kSecuritySealed;
}

bool Bq27220::Seal(int64_t now_us) {
    uint16_t operation_status = 0U;
    if (!ReadOperationStatus(operation_status, now_us)) {
        return false;
    }
    if ((operation_status & kSecurityMask) == kSecuritySealed) {
        return true;
    }
    if (!Control(kSealCommand, now_us)) {
        return false;
    }
    DelayMs(10U);
    return ReadOperationStatus(operation_status, now_us) && (operation_status & kSecurityMask) == kSecuritySealed;
}

bool Bq27220::WaitForConfigurationMode(bool enabled, int64_t now_us) {
    for (uint32_t attempt = 0U; attempt < kConfigurationPollAttempts; ++attempt) {
        uint16_t operation_status = 0U;
        if (!ReadOperationStatus(operation_status, now_us)) {
            return false;
        }
        if (((operation_status & kConfigurationModeMask) != 0U) == enabled) {
            return true;
        }
        DelayMs(100U);
    }
    ESP_LOGE(kTag, "timed out waiting for configuration mode %s", enabled ? "entry" : "exit");
    return false;
}

bool Bq27220::Control(uint16_t command, int64_t now_us) {
    const std::array<uint8_t, 2U> bytes{static_cast<uint8_t>(command), static_cast<uint8_t>(command >> 8U)};
    return WriteBytes(kControlRegister, bytes, now_us);
}

bool Bq27220::ReadOperationStatus(uint16_t& status, int64_t now_us) {
    return ReadRegister(kOperationStatusRegister, status, now_us);
}

bool Bq27220::ReadDataMemory(const Bq27220DataMemoryValue& parameter, uint16_t& value, int64_t now_us) {
    const std::array<uint8_t, 2U> address{static_cast<uint8_t>(parameter.address),
                                          static_cast<uint8_t>(parameter.address >> 8U)};
    if (!WriteBytes(kDataMemoryAddressRegister, address, now_us)) {
        return false;
    }
    DelayMs(10U);
    std::array<uint8_t, 2U> bytes{};
    const size_t width = Bq27220DataWidthBytes(parameter.width);
    if (!ReadBytes(kMacDataRegister, std::span<uint8_t>(bytes).first(width), now_us)) {
        return false;
    }
    value = parameter.width == Bq27220DataWidth::kU8
                ? bytes[0]
                : static_cast<uint16_t>(static_cast<uint16_t>(bytes[0]) << 8U) | bytes[1];
    return true;
}

bool Bq27220::WriteDataMemory(const Bq27220DataMemoryValue& parameter, int64_t now_us) {
    std::array<uint8_t, 4U> payload{static_cast<uint8_t>(parameter.address),
                                    static_cast<uint8_t>(parameter.address >> 8U)};
    const size_t width = Bq27220DataWidthBytes(parameter.width);
    if (parameter.width == Bq27220DataWidth::kU8) {
        payload[2] = static_cast<uint8_t>(parameter.value);
    } else {
        payload[2] = static_cast<uint8_t>(parameter.value >> 8U);
        payload[3] = static_cast<uint8_t>(parameter.value);
    }
    const std::span<const uint8_t> command(payload.data(), 2U + width);
    if (!WriteBytes(kDataMemoryAddressRegister, command, now_us)) {
        return false;
    }
    DelayMs(10U);
    const std::array<uint8_t, 2U> checksum{Bq27220Checksum(command), static_cast<uint8_t>(command.size() + 2U)};
    if (!WriteBytes(kMacChecksumRegister, checksum, now_us)) {
        return false;
    }
    DelayMs(10U);
    return true;
}

bool Bq27220::ReadRegister(uint8_t address, uint16_t& value, int64_t now_us) {
    uint8_t bytes[2]{};
    if (!ReadBytes(address, bytes, now_us)) {
        return false;
    }
    value = static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
    return true;
}

bool Bq27220::ReadBytes(uint8_t address, std::span<uint8_t> bytes, int64_t now_us) {
    const esp_err_t status =
        i2c_master_transmit_receive(device_, &address, sizeof(address), bytes.data(), bytes.size(), kI2cTimeoutMs);
    if (status != ESP_OK) {
        RecordFailure("read", address, status, now_us);
        return false;
    }
    RecordSuccess();
    return true;
}

bool Bq27220::WriteBytes(uint8_t address, std::span<const uint8_t> bytes, int64_t now_us) {
    std::array<uint8_t, 5U> transaction{};
    if (bytes.size() > transaction.size() - 1U) {
        return false;
    }
    transaction[0] = address;
    std::copy(bytes.begin(), bytes.end(), transaction.begin() + 1);
    const esp_err_t status = i2c_master_transmit(device_, transaction.data(), bytes.size() + 1U, kI2cTimeoutMs);
    if (status != ESP_OK) {
        RecordFailure("write", address, status, now_us);
        return false;
    }
    RecordSuccess();
    return true;
}

void Bq27220::RecordFailure(const char* operation, uint8_t address, esp_err_t status, int64_t now_us) {
    ++consecutive_read_failures_;
    if (consecutive_read_failures_ == 1U || consecutive_read_failures_ % 10U == 0U) {
        ESP_LOGW(kTag, "register 0x%02x %s failed: %s (consecutive=%lu)", address, operation, esp_err_to_name(status),
                 static_cast<unsigned long>(consecutive_read_failures_));
    }
    if (consecutive_read_failures_ >= 2U) {
        read_cooldown_until_us_ = now_us + kReadFailureCooldownUs;
    }
}

void Bq27220::RecordSuccess() {
    consecutive_read_failures_ = 0U;
    read_cooldown_until_us_ = 0;
}

}  // namespace micropixel::platform::drivers
