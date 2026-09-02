#include "platform/drivers/sensors/qmi8658.hpp"

#include <array>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "qmi8658";
constexpr uint8_t kAddress = 0x6aU;
constexpr uint8_t kWhoAmIRegister = 0x00U;
constexpr uint8_t kControl1Register = 0x02U;
constexpr uint8_t kAccelerationControlRegister = 0x03U;
constexpr uint8_t kAngularVelocityControlRegister = 0x04U;
constexpr uint8_t kControl7Register = 0x08U;
constexpr uint8_t kAccelerationXLowRegister = 0x35U;
constexpr uint8_t kAngularVelocityXLowRegister = 0x3bU;
constexpr uint8_t kResetRegister = 0x60U;
constexpr uint8_t kWhoAmI = 0x05U;
constexpr uint8_t kResetCommand = 0xb0U;
constexpr uint8_t kAddressAutoIncrement = 0x40U;
constexpr uint8_t kAccelerationEnable = 1U << 0U;
constexpr uint8_t kAngularVelocityEnable = 1U << 1U;
constexpr uint8_t kAccelerationRange4G = 1U << 4U;
constexpr uint8_t kAngularVelocityRange1024Dps = 6U << 4U;
constexpr uint32_t kI2cSpeedHz = 400000U;
constexpr int kTimeoutMs = 100;
constexpr float kGravityMetersPerSecondSquared = 9.80665F;
constexpr float kRadiansPerDegree = 0.017453292519943295F;
constexpr float kAccelerationCountsPerG = 8192.0F;
constexpr float kAngularVelocityCountsPerDegreePerSecond = 32.0F;

}  // namespace

Qmi8658::~Qmi8658() {
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
    }
}

esp_err_t Qmi8658::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || device_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (i2c_master_probe(bus, kAddress, kTimeoutMs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kAddress;
    config.scl_speed_hz = kI2cSpeedHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &device_), kTag, "add I2C device failed");

    uint8_t who_am_i = 0U;
    esp_err_t status = ReadRegisters(kWhoAmIRegister, &who_am_i, sizeof(who_am_i));
    if (status == ESP_OK && who_am_i != kWhoAmI) {
        status = ESP_ERR_INVALID_RESPONSE;
    }
    if (status == ESP_OK) {
        status = WriteRegister(kResetRegister, kResetCommand);
    }
    if (status == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(15));
        status = ReadRegisters(kWhoAmIRegister, &who_am_i, sizeof(who_am_i));
    }
    if (status == ESP_OK && who_am_i != kWhoAmI) {
        status = ESP_ERR_INVALID_RESPONSE;
    }
    if (status == ESP_OK) {
        status = WriteRegister(kControl1Register, kAddressAutoIncrement);
    }
    if (status == ESP_OK) {
        status = WriteRegister(kAccelerationControlRegister, acceleration_control_);
    }
    if (status == ESP_OK) {
        status = WriteRegister(kAngularVelocityControlRegister, angular_velocity_control_);
    }
    if (status == ESP_OK) {
        status = ApplyPowerState();
    }
    if (status != ESP_OK) {
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return status;
    }
    ESP_LOGI(kTag, "ready: WHO_AM_I=0x%02x address=0x%02x", who_am_i, kAddress);
    return ESP_OK;
}

esp_err_t Qmi8658::Configure(Kind kind, uint32_t interval_us) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    const bool previous_acceleration = acceleration_active_;
    const bool previous_angular_velocity = angular_velocity_active_;
    const uint8_t previous_control = kind == Kind::kAcceleration ? acceleration_control_ : angular_velocity_control_;
    uint8_t& control = kind == Kind::kAcceleration ? acceleration_control_ : angular_velocity_control_;
    control =
        static_cast<uint8_t>((kind == Kind::kAcceleration ? kAccelerationRange4G : kAngularVelocityRange1024Dps) |
                             (kind == Kind::kAcceleration ? AccelerationOdr(interval_us) : GyroscopeOdr(interval_us)));
    if (kind == Kind::kAcceleration) {
        acceleration_active_ = true;
    } else {
        angular_velocity_active_ = true;
    }
    const uint8_t register_address =
        kind == Kind::kAcceleration ? kAccelerationControlRegister : kAngularVelocityControlRegister;
    esp_err_t status = WriteRegister(register_address, control);
    if (status == ESP_OK) {
        status = ApplyPowerState();
    }
    if (status != ESP_OK) {
        acceleration_active_ = previous_acceleration;
        angular_velocity_active_ = previous_angular_velocity;
        control = previous_control;
        (void)WriteRegister(register_address, control);
        (void)ApplyPowerState();
    }
    return status;
}

esp_err_t Qmi8658::Suspend(Kind kind) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (kind == Kind::kAcceleration) {
        acceleration_active_ = false;
    } else {
        angular_velocity_active_ = false;
    }
    return ApplyPowerState();
}

esp_err_t Qmi8658::Read(Kind kind, float (&values)[3]) {
    if (!available()) {
        return ESP_ERR_INVALID_STATE;
    }
    std::array<uint8_t, 6U> bytes{};
    const uint8_t address = kind == Kind::kAcceleration ? kAccelerationXLowRegister : kAngularVelocityXLowRegister;
    ESP_RETURN_ON_ERROR(ReadRegisters(address, bytes.data(), bytes.size()), kTag, "read vector failed");
    const float scale = kind == Kind::kAcceleration ? kGravityMetersPerSecondSquared / kAccelerationCountsPerG
                                                    : kRadiansPerDegree / kAngularVelocityCountsPerDegreePerSecond;
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        const int16_t raw =
            static_cast<int16_t>((static_cast<uint16_t>(bytes[axis * 2U + 1U]) << 8U) | bytes[axis * 2U]);
        values[axis] = static_cast<float>(raw) * scale;
    }
    return ESP_OK;
}

esp_err_t Qmi8658::WriteRegister(uint8_t address, uint8_t value) {
    const uint8_t transaction[]{address, value};
    return device_ == nullptr ? ESP_ERR_INVALID_STATE
                              : i2c_master_transmit(device_, transaction, sizeof(transaction), kTimeoutMs);
}

esp_err_t Qmi8658::ReadRegisters(uint8_t address, uint8_t* data, size_t length) {
    if (device_ == nullptr || data == nullptr || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(device_, &address, sizeof(address), data, length, kTimeoutMs);
}

esp_err_t Qmi8658::ApplyPowerState() {
    const uint8_t control = static_cast<uint8_t>((acceleration_active_ ? kAccelerationEnable : 0U) |
                                                 (angular_velocity_active_ ? kAngularVelocityEnable : 0U));
    return WriteRegister(kControl7Register, control);
}

uint8_t Qmi8658::AccelerationOdr(uint32_t interval_us) {
    if (interval_us <= 2500U) {
        return 0x04U;  // 500 Hz
    }
    if (interval_us <= 5000U) {
        return 0x05U;  // 250 Hz
    }
    if (interval_us <= 10000U) {
        return 0x06U;  // 125 Hz
    }
    if (interval_us <= 20000U) {
        return 0x07U;  // 62.5 Hz
    }
    return 0x08U;  // 31.25 Hz
}

uint8_t Qmi8658::GyroscopeOdr(uint32_t interval_us) {
    if (interval_us <= 2500U) {
        return 0x04U;  // 448.4 Hz
    }
    if (interval_us <= 5000U) {
        return 0x05U;  // 224.2 Hz
    }
    if (interval_us <= 10000U) {
        return 0x06U;  // 112.1 Hz
    }
    if (interval_us <= 20000U) {
        return 0x07U;  // 56.05 Hz
    }
    return 0x08U;  // 28.025 Hz
}

esp_err_t Qmi8658::Vector::Initialize(i2c_master_bus_handle_t bus) { return sensor_.Initialize(bus); }

esp_err_t Qmi8658::Vector::Configure(uint32_t interval_us) { return sensor_.Configure(kind_, interval_us); }

esp_err_t Qmi8658::Vector::Suspend() { return sensor_.Suspend(kind_); }

esp_err_t Qmi8658::Vector::Read(float (&values)[3]) { return sensor_.Read(kind_, values); }

}  // namespace micropixel::platform::drivers
