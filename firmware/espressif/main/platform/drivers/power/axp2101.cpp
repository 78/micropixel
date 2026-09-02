#include "platform/drivers/power/axp2101.hpp"

#include "esp_check.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "axp2101";
constexpr uint8_t kStatus0Register = 0x00U;
constexpr uint8_t kStatus1Register = 0x01U;
constexpr uint8_t kPowerOffRegister = 0x10U;
constexpr uint8_t kLdoEnableRegister = 0x90U;
constexpr uint8_t kBatteryPercentRegister = 0xa4U;
constexpr int kI2cTimeoutMs = 100;

}  // namespace

Axp2101::~Axp2101() {
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
    }
}

esp_err_t Axp2101::Attach(i2c_master_bus_handle_t bus, uint8_t address) {
    if (bus == nullptr || address == 0U || device_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = address;
    config.scl_speed_hz = 400000U;
    return i2c_master_bus_add_device(bus, &config, &device_);
}

esp_err_t Axp2101::SetLdoVoltage(uint8_t voltage_register, uint8_t enable_mask, int voltage_mv) {
    if (device_ == nullptr || voltage_register < 0x92U || voltage_register > 0x99U || enable_mask == 0U ||
        voltage_mv < 0 || voltage_mv > 3300 || (voltage_mv != 0 && voltage_mv < 500)) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t encoded = voltage_mv == 0 ? 0U : static_cast<uint8_t>((voltage_mv - 500) / 100);
    ESP_RETURN_ON_ERROR(WriteRegister(voltage_register, encoded), kTag, "write LDO voltage failed");
    uint8_t enabled = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(kLdoEnableRegister, enabled), kTag, "read LDO enable state failed");
    const uint8_t next =
        voltage_mv == 0 ? static_cast<uint8_t>(enabled & ~enable_mask) : static_cast<uint8_t>(enabled | enable_mask);
    return next == enabled ? ESP_OK : WriteRegister(kLdoEnableRegister, next);
}

esp_err_t Axp2101::ReadBattery(Axp2101BatterySample& sample) {
    sample = {};
    uint8_t status0 = 0U;
    uint8_t status1 = 0U;
    uint8_t percent = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(kStatus0Register, status0), kTag, "read status 0 failed");
    ESP_RETURN_ON_ERROR(ReadRegister(kStatus1Register, status1), kTag, "read status 1 failed");
    ESP_RETURN_ON_ERROR(ReadRegister(kBatteryPercentRegister, percent), kTag, "read battery SOC failed");
    const uint8_t direction = static_cast<uint8_t>((status1 >> 5U) & 0x03U);
    sample.percent = percent <= 100U ? percent : 0U;
    sample.battery_present = (status0 & (1U << 3U)) != 0U && percent <= 100U;
    sample.external_power_connected = (status0 & (1U << 5U)) != 0U;
    sample.charging = direction == 1U;
    sample.discharging = direction == 2U;
    return ESP_OK;
}

esp_err_t Axp2101::PowerOff() {
    uint8_t value = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(kPowerOffRegister, value), kTag, "read power-off control failed");
    return WriteRegister(kPowerOffRegister, static_cast<uint8_t>(value | 0x01U));
}

esp_err_t Axp2101::ReadRegister(uint8_t reg, uint8_t& value) const {
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(device_, &reg, sizeof(reg), &value, sizeof(value), kI2cTimeoutMs);
}

esp_err_t Axp2101::WriteRegister(uint8_t reg, uint8_t value) const {
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t bytes[]{reg, value};
    return i2c_master_transmit(device_, bytes, sizeof(bytes), kI2cTimeoutMs);
}

}  // namespace micropixel::platform::drivers
