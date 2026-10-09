// SPDX-License-Identifier: Apache-2.0
#include "platform/drivers/rtc/rx8130.hpp"

#include "esp_log.h"
#include "esp_timer.h"

namespace micropixel::platform::drivers {
namespace {

constexpr char kTag[] = "rx8130";
constexpr uint32_t kI2cSpeedHz = 400'000U;
constexpr int kTimeoutMs = 50;
constexpr int64_t kProbeRetryUs = 1000'000;

// Registers (see the RX8130CE register datasheet: 0x10..0x16 calendar,
// 0x1D flags, 0x1E control 0, 0x1F control 1).
constexpr uint8_t kRegisterSecond = 0x10U;
constexpr uint8_t kRegisterFlag = 0x1DU;
constexpr uint8_t kRegisterControl0 = 0x1EU;
constexpr uint8_t kRegisterControl1 = 0x1FU;

// Flag register bit 1: voltage loss, set when the backup supply dropped.
constexpr uint8_t kFlagVoltageLoss = 1U << 1U;
// Control register 0 bit 6 stops the timekeeping counter; bit 7 (TEST) must
// stay 0.
constexpr uint8_t kControl0Stop = 1U << 6U;
// Control register 1: INIEN (power switchover) and CHGEN (backup charge).
constexpr uint8_t kControl1BackupMask = (1U << 5U) | (1U << 4U);

}  // namespace

Rx8130::~Rx8130() {
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
    }
}

esp_err_t Rx8130::Bind(i2c_master_bus_handle_t bus, Rx8130Config config) {
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    bus_ = bus;
    config_ = config;
    return ESP_OK;
}

bool Rx8130::Prepare() {
    if (device_ != nullptr) {
        return true;
    }
    const int64_t now_us = esp_timer_get_time();
    if (bus_ == nullptr || now_us < next_probe_us_) {
        return false;
    }
    if (i2c_master_probe(bus_, config_.address, kTimeoutMs) != ESP_OK) {
        next_probe_us_ = now_us + kProbeRetryUs;
        if (!probe_logged_) {
            ESP_LOGW(kTag, "no RX8130CE at 0x%02x", config_.address);
            probe_logged_ = true;
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
    return true;
}

void Rx8130::Drop(const char* operation, esp_err_t status) {
    ESP_LOGW(kTag, "%s failed: %s", operation, esp_err_to_name(status));
    if (device_ != nullptr) {
        (void)i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    next_probe_us_ = esp_timer_get_time() + kProbeRetryUs;
}

esp_err_t Rx8130::ReadRegisters(uint8_t address, uint8_t* out, size_t size) {
    if (out == nullptr || size == 0U || !Prepare()) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(device_, &address, sizeof(address), out, size, kTimeoutMs);
}

esp_err_t Rx8130::WriteRegisters(uint8_t address, const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0U || !Prepare()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[1U + rx8130::kCalendarRegisterSize] = {};
    if (size > sizeof(payload) - 1U) {
        return ESP_ERR_INVALID_SIZE;
    }
    payload[0] = address;
    for (size_t index = 0U; index < size; ++index) {
        payload[index + 1U] = data[index];
    }
    return i2c_master_transmit(device_, payload, size + 1U, kTimeoutMs);
}

esp_err_t Rx8130::UpdateRegister(uint8_t address, uint8_t mask, uint8_t value) {
    uint8_t current = 0U;
    const esp_err_t read_status = ReadRegisters(address, &current, 1U);
    if (read_status != ESP_OK) {
        return read_status;
    }
    const auto updated = static_cast<uint8_t>((current & ~mask) | (value & mask));
    if (updated == current) {
        return ESP_OK;
    }
    return WriteRegisters(address, &updated, 1U);
}

esp_err_t Rx8130::Configure() {
    if (!Prepare()) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t cleared = 0U;
    esp_err_t status = WriteRegisters(kRegisterControl0, &cleared, 1U);
    if (status == ESP_OK) {
        status = UpdateRegister(kRegisterControl1, kControl1BackupMask, kControl1BackupMask);
    }
    if (status != ESP_OK) {
        Drop("configuration write", status);
        return status;
    }
    ESP_LOGI(kTag, "ready: RX8130CE at 0x%02x, backup charging enabled", config_.address);
    return ESP_OK;
}

bool Rx8130::VoltageLossDetected() {
    uint8_t flags = 0U;
    const esp_err_t status = ReadRegisters(kRegisterFlag, &flags, 1U);
    if (status != ESP_OK) {
        Drop("flag read", status);
        return false;
    }
    return (flags & kFlagVoltageLoss) != 0U;
}

esp_err_t Rx8130::ClearVoltageLoss() {
    uint8_t flags = 0U;
    const esp_err_t status = ReadRegisters(kRegisterFlag, &flags, 1U);
    if (status != ESP_OK) {
        Drop("flag read", status);
        return status;
    }
    if ((flags & kFlagVoltageLoss) == 0U) {
        return ESP_OK;
    }
    const esp_err_t cleared = UpdateRegister(kRegisterFlag, kFlagVoltageLoss, 0U);
    if (cleared != ESP_OK) {
        Drop("flag write", cleared);
        return cleared;
    }
    ESP_LOGI(kTag, "voltage-loss flag cleared: the calendar is trustworthy again");
    return ESP_OK;
}

esp_err_t Rx8130::ReadCalendar(rx8130::Fields& fields) {
    // One burst keeps the seven registers consistent: the chip holds the carry
    // from the first access to the last.
    uint8_t raw[rx8130::kCalendarRegisterSize] = {};
    const esp_err_t status = ReadRegisters(kRegisterSecond, raw, sizeof(raw));
    if (status != ESP_OK) {
        Drop("calendar read", status);
        return status;
    }
    fields = {raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]};
    return ESP_OK;
}

esp_err_t Rx8130::WriteCalendar(const rx8130::Fields& fields) {
    esp_err_t status = UpdateRegister(kRegisterControl0, kControl0Stop, kControl0Stop);
    if (status == ESP_OK) {
        const uint8_t raw[rx8130::kCalendarRegisterSize] = {fields.second, fields.minute, fields.hour, fields.weekday,
                                                            fields.day,    fields.month,  fields.year};
        status = WriteRegisters(kRegisterSecond, raw, sizeof(raw));
    }
    if (status == ESP_OK) {
        status = UpdateRegister(kRegisterControl0, kControl0Stop, 0U);
    }
    if (status != ESP_OK) {
        Drop("calendar write", status);
        return status;
    }
    return ESP_OK;
}

}  // namespace micropixel::platform::drivers
