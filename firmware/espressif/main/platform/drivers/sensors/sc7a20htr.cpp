#include "platform/drivers/sensors/sc7a20htr.hpp"

#include "esp_log.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "sc7a20htr";
constexpr uint8_t kAddress = 0x19U;
constexpr uint8_t kWhoAmIRegister = 0x0fU;
constexpr uint8_t kControl1Register = 0x20U;
constexpr uint8_t kControl4Register = 0x23U;
constexpr uint8_t kOutputXLowRegister = 0x28U;
constexpr uint8_t kAutoIncrement = 0x80U;
constexpr uint8_t kWhoAmI = 0x11U;
constexpr uint8_t kPowerDown = 0x07U;
constexpr uint8_t kSample10Hz = 0x27U;
constexpr uint8_t kSample50Hz = 0x47U;
constexpr uint8_t kSample100Hz = 0x57U;
constexpr uint8_t kSample200Hz = 0x67U;
constexpr uint8_t kSample400Hz = 0x77U;
constexpr uint8_t kControl4Value = 0x88U;
constexpr uint32_t kI2cSpeedHz = 100000U;
constexpr int kTimeoutMs = 50;
constexpr float kMetersPerSecondSquaredPerMilliG = 0.00980665F;

esp_err_t Write(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) {
    const uint8_t transaction[] = {address, value};
    return i2c_master_transmit(device, transaction, sizeof(transaction), kTimeoutMs);
}

}  // namespace

esp_err_t Sc7a20htr::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || i2c_master_probe(bus, kAddress, kTimeoutMs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = kI2cSpeedHz;
    esp_err_t status = i2c_master_bus_add_device(bus, &config, &device_);
    uint8_t who_am_i = 0U;
    if (status == ESP_OK) {
        status = i2c_master_transmit_receive(device_, &kWhoAmIRegister, sizeof(kWhoAmIRegister), &who_am_i,
                                             sizeof(who_am_i), kTimeoutMs);
    }
    if (status == ESP_OK && who_am_i != kWhoAmI) {
        status = ESP_ERR_INVALID_RESPONSE;
    }
    if (status == ESP_OK) {
        status = Write(device_, kControl4Register, kControl4Value);
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
    ESP_LOGI(kTag, "ready: WHO_AM_I=0x%02x", who_am_i);
    return ESP_OK;
}

esp_err_t Sc7a20htr::Configure(uint32_t interval_us) {
    uint8_t control = kSample10Hz;
    if (interval_us <= 2500U) {
        control = kSample400Hz;
    } else if (interval_us <= 5000U) {
        control = kSample200Hz;
    } else if (interval_us <= 10000U) {
        control = kSample100Hz;
    } else if (interval_us <= 20000U) {
        control = kSample50Hz;
    }
    return device_ == nullptr ? ESP_ERR_INVALID_STATE : Write(device_, kControl1Register, control);
}

esp_err_t Sc7a20htr::Suspend() {
    return device_ == nullptr ? ESP_ERR_INVALID_STATE : Write(device_, kControl1Register, kPowerDown);
}

esp_err_t Sc7a20htr::Read(float (&values)[3]) {
    uint8_t address = kOutputXLowRegister | kAutoIncrement;
    uint8_t bytes[6]{};
    if (device_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t status =
        i2c_master_transmit_receive(device_, &address, sizeof(address), bytes, sizeof(bytes), kTimeoutMs);
    if (status != ESP_OK) {
        return status;
    }
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        const int16_t raw =
            static_cast<int16_t>((static_cast<uint16_t>(bytes[axis * 2U + 1U]) << 8U) | bytes[axis * 2U]);
        values[axis] = static_cast<float>(raw >> 4) * kMetersPerSecondSquaredPerMilliG;
    }
    return ESP_OK;
}

}  // namespace micropixel::platform::drivers
