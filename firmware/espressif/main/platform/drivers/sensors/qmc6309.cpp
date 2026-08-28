#include "platform/drivers/sensors/qmc6309.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "qmc6309";
constexpr uint8_t kAddress = 0x7cU;
constexpr uint8_t kChipIdRegister = 0x00U;
constexpr uint8_t kOutputXLowRegister = 0x01U;
constexpr uint8_t kControl1Register = 0x0aU;
constexpr uint8_t kControl2Register = 0x0bU;
constexpr uint8_t kChipId = 0x90U;
constexpr uint8_t kLowPowerOsr = 0x18U;
constexpr uint8_t kNormal10Hz = 0x10U;
constexpr uint8_t kNormal50Hz = 0x20U;
constexpr uint8_t kNormal100Hz = 0x30U;
constexpr uint8_t kNormal200Hz = 0x40U;
constexpr uint8_t kSoftReset = 0x80U;
constexpr uint8_t kSuspendMode = 0x00U;
constexpr uint8_t kNormalMode = 0x01U;
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kTimeoutMs = 50;
constexpr float kMicroteslaPerLsb = 0.1F;

esp_err_t Write(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) {
    const uint8_t transaction[] = {address, value};
    return i2c_master_transmit(device, transaction, sizeof(transaction), kTimeoutMs);
}

}  // namespace

esp_err_t Qmc6309::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || i2c_master_probe(bus, kAddress, kTimeoutMs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = kI2cSpeedHz;
    esp_err_t status = i2c_master_bus_add_device(bus, &config, &device_);
    uint8_t chip_id = 0U;
    if (status == ESP_OK) {
        status = i2c_master_transmit_receive(device_, &kChipIdRegister, sizeof(kChipIdRegister), &chip_id,
                                             sizeof(chip_id), kTimeoutMs);
    }
    if (status == ESP_OK && chip_id != kChipId) {
        status = ESP_ERR_INVALID_RESPONSE;
    }
    if (status == ESP_OK) {
        status = Write(device_, kControl2Register, kSoftReset);
        vTaskDelay(pdMS_TO_TICKS(25U));
    }
    if (status == ESP_OK) {
        status = Write(device_, kControl2Register, 0U);
        vTaskDelay(pdMS_TO_TICKS(25U));
    }
    if (status == ESP_OK) {
        status = Suspend();
    }
    if (status != ESP_OK) {
        if (device_ != nullptr) {
            (void)i2c_master_bus_rm_device(device_);
            device_ = nullptr;
        }
        return status;
    }
    ESP_LOGI(kTag, "ready: CHIP_ID=0x%02x", chip_id);
    return ESP_OK;
}

esp_err_t Qmc6309::Configure(uint32_t interval_us) {
    uint8_t control2 = kNormal10Hz;
    if (interval_us <= 5000U) {
        control2 = kNormal200Hz;
    } else if (interval_us <= 10000U) {
        control2 = kNormal100Hz;
    } else if (interval_us <= 20000U) {
        control2 = kNormal50Hz;
    }
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t status = Suspend();
    if (status == ESP_OK) {
        status = Write(device_, kControl2Register, control2);
    }
    if (status == ESP_OK) {
        status = Write(device_, kControl1Register, kLowPowerOsr | kNormalMode);
    }
    return status;
}

esp_err_t Qmc6309::Suspend() {
    return device_ == nullptr ? ESP_ERR_INVALID_STATE : Write(device_, kControl1Register, kSuspendMode);
}

esp_err_t Qmc6309::Read(float (&values)[3]) {
    uint8_t bytes[6]{};
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t status = i2c_master_transmit_receive(device_, &kOutputXLowRegister, sizeof(kOutputXLowRegister),
                                                         bytes, sizeof(bytes), kTimeoutMs);
    if (status != ESP_OK) {
        return status;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        const int16_t raw =
            static_cast<int16_t>((static_cast<uint16_t>(bytes[axis * 2U + 1U]) << 8U) | bytes[axis * 2U]);
        values[axis] = static_cast<float>(raw) * kMicroteslaPerLsb;
    }
    return ESP_OK;
}

}  // namespace micropixel::platform::drivers
