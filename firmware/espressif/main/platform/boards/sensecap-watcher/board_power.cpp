// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/board_power.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/sensecap-watcher/board_config.hpp"

// The rail directions, the startup order and the active levels follow the board
// support package published for this hardware, which is the authority for it:
//   Seeed-Studio/SenseCAP-Watcher-Firmware, components/sensecap-watcher
// The upstream direction masks contradict their own trailing comments, but the
// mask values are self-consistent once expander pin n maps to bit n, and both
// the vendor BSP and the xiaozhi board implement the same values. See
// THIRD_PARTY_NOTICES.md.
namespace micropixel::platform::sensecap_watcher {
namespace {

constexpr char kTag[] = "sensecap_watcher";

// PCA95xx register map: input 0x00/0x01, output 0x02/0x03, configuration
// 0x06/0x07. A configuration bit set to 1 selects an input.
constexpr uint8_t kRegisterInputPort0 = 0x00U;
constexpr uint8_t kRegisterOutputPort0 = 0x02U;
constexpr uint8_t kRegisterConfigurationPort0 = 0x06U;

// Port 0 is entirely input (charge, standby, VBUS, knob, SD detect, touch
// interrupt, AI sync and reset). Port 1 carries the rails, except P1.5, which
// is the battery-detect input.
constexpr uint8_t kConfigurationPort0 = 0xFFU;
constexpr uint8_t kConfigurationPort1 = 0x20U;

// Every output is low after the expander's own power-on reset; only the system
// rail comes up first, because the remaining rails are only safe once the SoC is
// powered. P1.2 is that system rail, so it is also the power-off latch.
constexpr uint8_t kOutputPort1Off = 0x00U;
constexpr uint8_t kOutputPort1SystemRailOnly = 0x04U;  // P1.2
constexpr uint8_t kOutputPort1StartupRails = 0xDFU;    // P1.0-P1.4, P1.6, P1.7

constexpr uint32_t kSystemRailDelayMs = 100U;
constexpr uint32_t kStartupRailDelayMs = 50U;

// The expander inputs are active low. Polarity is applied by each caller rather
// than encoded in a mask here, because the lines sit on different ports.

}  // namespace

esp_err_t BoardPower::Initialize() {
    if (expander_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t status = InitializeControlI2c();
    if (status != ESP_OK) {
        return status;
    }
    status = AddExpanderDevice();
    if (status == ESP_OK) {
        status = ConfigureDirections();
    }
    if (status == ESP_OK) {
        status = SequenceStartup();
    }
    return status;
}

esp_err_t BoardPower::InitializeControlI2c() {
    i2c_master_bus_config_t config{};
    config.i2c_port = board::kControlI2cPort;
    config.sda_io_num = board::kControlSda;
    config.scl_io_num = board::kControlScl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.intr_priority = 0;
    config.trans_queue_depth = 0;
    config.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&config, &control_bus_);
}

esp_err_t BoardPower::AddExpanderDevice() {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = board::kIoExpanderI2cAddress;
    config.scl_speed_hz = board::kControlI2cClockHz;
    const esp_err_t status = i2c_master_bus_add_device(control_bus_, &config, &expander_);
    if (status == ESP_OK) {
        ESP_LOGI(kTag, "control bus ready: PCA95xx expander at 0x%02X", board::kIoExpanderI2cAddress);
    }
    return status;
}

esp_err_t BoardPower::ConfigureDirections() const {
    esp_err_t status = WriteRegister(kRegisterConfigurationPort0, kConfigurationPort0);
    if (status != ESP_OK) {
        return status;
    }
    return WriteRegister(kRegisterConfigurationPort0 + 1U, kConfigurationPort1);
}

esp_err_t BoardPower::SequenceStartup() {
    // Raise the system rail first instead of clearing the whole output port
    // beforehand. Clearing it would drive P1.2 low and cut the rail that powers
    // the running SoC; the vendor sequence survives that only because the
    // interruption is shorter than the rail's hold-up capacitance. The latch
    // value reached here is the same either way, and the rails that must stay
    // down are already down when the expander resets.
    esp_err_t status = WriteRegister(kRegisterOutputPort0 + 1U, kOutputPort1SystemRailOnly);
    if (status != ESP_OK) {
        return status;
    }
    vTaskDelay(pdMS_TO_TICKS(kSystemRailDelayMs));
    status = WriteRegister(kRegisterOutputPort0 + 1U, kOutputPort1StartupRails);
    if (status != ESP_OK) {
        return status;
    }
    vTaskDelay(pdMS_TO_TICKS(kStartupRailDelayMs));
    ESP_LOGI(kTag, "power sequencing complete: system rail up, peripheral rails latched");
    return ESP_OK;
}

bool BoardPower::IsCharging() {
    bool level = false;
    if (ReadPinLevel(board::kExpanderPinVbusInDetect, level) != ESP_OK) {
        return false;
    }
    return !level;
}

bool BoardPower::IsBatteryPresent() {
    bool level = false;
    if (ReadPinLevel(board::kExpanderPinBatteryDetect, level) != ESP_OK) {
        return false;
    }
    return !level;
}

esp_err_t BoardPower::PowerOff() {
    const esp_err_t status = WriteRegister(kRegisterOutputPort0 + 1U, kOutputPort1Off);
    if (status == ESP_OK) {
        ESP_LOGI(kTag, "system rail cut; board is not powered by software any more");
    }
    return status;
}

esp_err_t BoardPower::WriteRegister(uint8_t reg, uint8_t value) const {
    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(expander_, payload, sizeof(payload), -1);
}

esp_err_t BoardPower::ReadRegister(uint8_t reg, uint8_t& value) const {
    return i2c_master_transmit_receive(expander_, &reg, 1U, &value, 1U, -1);
}

esp_err_t BoardPower::ReadPinLevel(uint8_t pin, bool& level) const {
    // Input port p answers on register 0x00 + p; the line is bit (pin % 8).
    uint8_t value = 0U;
    const esp_err_t status = ReadRegister(kRegisterInputPort0 + (pin / 8U), value);
    if (status != ESP_OK) {
        return status;
    }
    level = (value & (1U << (pin % 8U))) != 0U;
    return ESP_OK;
}

}  // namespace micropixel::platform::sensecap_watcher
