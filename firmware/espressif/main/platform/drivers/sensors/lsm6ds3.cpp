// SPDX-License-Identifier: Apache-2.0
#include "platform/drivers/sensors/lsm6ds3.hpp"

#include <array>

#include "esp_log.h"

namespace micropixel::platform::drivers {
namespace {
constexpr char kTag[] = "lsm6ds3";
constexpr int kTimeoutMs = 10;
constexpr uint8_t kAccelerationControl = 0x10U;
constexpr uint8_t kGyroscopeControl = 0x11U;
}  // namespace

Lsm6ds3::~Lsm6ds3() {
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
    }
}

esp_err_t Lsm6ds3::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr || device_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (const uint8_t address : {0x6aU, 0x6bU}) {
        if (i2c_master_probe(bus, address, kTimeoutMs) != ESP_OK) {
            continue;
        }
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 400000U;
        const esp_err_t attach_status = i2c_master_bus_add_device(bus, &config, &device_);
        if (attach_status != ESP_OK) {
            return attach_status;
        }
        uint8_t identity = 0U;
        esp_err_t status = ReadRegisters(0x0fU, &identity, 1U);
        if (status == ESP_OK && identity != 0x69U && identity != 0x6aU) {
            status = ESP_ERR_INVALID_RESPONSE;
        }
        if (status == ESP_OK) {
            status = WriteRegister(0x12U, 0x44U);  // BDU + register auto-increment.
        }
        if (status == ESP_OK) {
            status = WriteRegister(kAccelerationControl, 0U);
        }
        if (status == ESP_OK) {
            status = WriteRegister(kGyroscopeControl, 0U);
        }
        if (status == ESP_OK) {
            ESP_LOGI(kTag, "ready: WHO_AM_I=0x%02x address=0x%02x", identity, address);
            return ESP_OK;
        }
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        if (identity == 0x69U || identity == 0x6aU) {
            return status;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

uint8_t Lsm6ds3::OutputDataRate(uint32_t interval_us) {
    if (interval_us < 4808U) {
        return 0x60U;  // 416 Hz; ODR is in bits [7:4].
    }
    if (interval_us < 9616U) {
        return 0x50U;  // 208 Hz.
    }
    if (interval_us < 19231U) {
        return 0x40U;  // 104 Hz.
    }
    if (interval_us < 38462U) {
        return 0x30U;  // 52 Hz.
    }
    if (interval_us < 80000U) {
        return 0x20U;  // 26 Hz.
    }
    return 0x10U;  // 12.5 Hz.
}

esp_err_t Lsm6ds3::Configure(Kind kind, uint32_t interval_us) {
    if (interval_us < 2404U) {
        return ESP_ERR_INVALID_ARG;
    }
    // Native ranges: +/-2 g and +/-245 (DS3) / +/-250 (TR-C) dps.
    return WriteRegister(kind == Kind::kAcceleration ? kAccelerationControl : kGyroscopeControl,
                         OutputDataRate(interval_us));
}

esp_err_t Lsm6ds3::Suspend(Kind kind) {
    return WriteRegister(kind == Kind::kAcceleration ? kAccelerationControl : kGyroscopeControl, 0U);
}

esp_err_t Lsm6ds3::Read(Kind kind, float (&values)[3]) {
    std::array<uint8_t, 6U> bytes{};
    const esp_err_t status = ReadRegisters(kind == Kind::kAcceleration ? 0x28U : 0x22U, bytes.data(), bytes.size());
    if (status != ESP_OK) {
        return status;
    }
    // Datasheet sensitivity, converted to Device contract SI units.
    const float scale = kind == Kind::kAcceleration ? 0.000061F * 9.80665F : 0.00875F * 0.017453292519943295F;
    for (uint32_t axis = 0U; axis < 3U; ++axis) {
        const int16_t raw =
            static_cast<int16_t>((static_cast<uint16_t>(bytes[axis * 2U + 1U]) << 8U) | bytes[axis * 2U]);
        values[axis] = static_cast<float>(raw) * scale;
    }
    return ESP_OK;
}

esp_err_t Lsm6ds3::WriteRegister(uint8_t address, uint8_t value) {
    const uint8_t bytes[]{address, value};
    return device_ == nullptr ? ESP_ERR_INVALID_STATE : i2c_master_transmit(device_, bytes, sizeof(bytes), kTimeoutMs);
}

esp_err_t Lsm6ds3::ReadRegisters(uint8_t address, uint8_t* data, size_t length) {
    return device_ == nullptr ? ESP_ERR_INVALID_STATE
                              : i2c_master_transmit_receive(device_, &address, 1U, data, length, kTimeoutMs);
}

esp_err_t Lsm6ds3::Vector::Initialize(i2c_master_bus_handle_t bus) { return sensor_.Initialize(bus); }
esp_err_t Lsm6ds3::Vector::Configure(uint32_t interval_us) { return sensor_.Configure(kind_, interval_us); }
esp_err_t Lsm6ds3::Vector::Suspend() { return sensor_.Suspend(kind_); }
esp_err_t Lsm6ds3::Vector::Read(float (&values)[3]) { return sensor_.Read(kind_, values); }
}  // namespace micropixel::platform::drivers
