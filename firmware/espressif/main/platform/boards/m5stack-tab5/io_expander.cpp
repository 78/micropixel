// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/io_expander.hpp"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_ioexp";

constexpr uint8_t kAddresses[2]{
    0x43U,  // U1, address pin low
    0x44U,  // U2, address pin high
};
constexpr uint32_t kTransferTimeoutMs = 50U;

// PI4IOE5V6416 register map (matches the vendor BSP).
constexpr uint8_t kRegisterChipReset = 0x01U;
// Input status (PI4IO_REG_IN_STA in the vendor BSP): the only register that
// reports pin levels, including pins driven as outputs.
constexpr uint8_t kRegisterInputPort = 0x0FU;
constexpr uint8_t kRegisterIoDirection = 0x03U;
constexpr uint8_t kRegisterOutput = 0x05U;
constexpr uint8_t kRegisterOutputHighImpedance = 0x07U;
constexpr uint8_t kRegisterInputDefault = 0x09U;
constexpr uint8_t kRegisterPullEnable = 0x0BU;
constexpr uint8_t kRegisterPullSelect = 0x0DU;
constexpr uint8_t kRegisterInterruptMask = 0x11U;

constexpr uint8_t kLcdResetBit = 4U;
constexpr uint8_t kTouchResetBit = 5U;

}  // namespace

esp_err_t Pi4ioeExpander::WriteRegister(uint8_t unit, uint8_t reg, uint8_t value) {
    if (unit >= kDeviceCount || devices_[unit] == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t bytes[]{reg, value};
    return i2c_master_transmit(devices_[unit], bytes, sizeof(bytes), kTransferTimeoutMs);
}

esp_err_t Pi4ioeExpander::ReadRegister(uint8_t unit, uint8_t reg, uint8_t* value) {
    if (unit >= kDeviceCount || devices_[unit] == nullptr || value == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(devices_[unit], &reg, sizeof(reg), value, sizeof(*value), kTransferTimeoutMs);
}

esp_err_t Pi4ioeExpander::UpdateOutput(uint8_t unit, uint8_t mask, uint8_t set) {
    uint8_t value = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(unit, kRegisterOutput, &value), kTag, "read output latch failed");
    value = static_cast<uint8_t>((value & ~mask) | (set & mask));
    return WriteRegister(unit, kRegisterOutput, value);
}

esp_err_t Pi4ioeExpander::Initialize(i2c_master_bus_handle_t bus) {
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t unit = 0U; unit < kDeviceCount; ++unit) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = kAddresses[unit];
        config.scl_speed_hz = 400'000U;
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &devices_[unit]), kTag,
                            "attach PI4IOE at 0x%02x failed", kAddresses[unit]);
        ESP_RETURN_ON_ERROR(WriteRegister(unit, kRegisterChipReset, 0xFFU), kTag, "reset PI4IOE at 0x%02x failed",
                            kAddresses[unit]);
        // The reset register latches; read it back to clear the status.
        uint8_t reset_status = 0U;
        ESP_RETURN_ON_ERROR(ReadRegister(unit, kRegisterChipReset, &reset_status), kTag,
                            "clear PI4IOE reset status failed");
    }

    // U1: P1 (speaker enable), P2 (EXT 5V), P4 (LCD_RST), P5 (TP_RST) and P6
    // (camera reset) are push-pull outputs driven high; P7 stays the headphone
    // detect input. LCD_RST must be driven high by the expander instead of
    // being released to an input pull-up: on the ILI9881C + GT911 panel the
    // touch controller shares that reset net and needs a real 3.3 V level,
    // otherwise it stays in reset while still answering on I2C (the panel
    // renders and touch never reports). The vendor only switched to the weak
    // pull-up for ST7123/ST7121 panels, whose LCD_RST is a 1.8 V domain.
    ESP_RETURN_ON_ERROR(WriteRegister(0U, kRegisterPullSelect, 0b0111'1111U), kTag, "U1 pull select failed");
    ESP_RETURN_ON_ERROR(WriteRegister(0U, kRegisterPullEnable, 0b0111'1111U), kTag, "U1 pull enable failed");
    // P1 (speaker amplifier enable) stays low here: the audio sink turns it on
    // only while it plays, so the codec's power-up transient never reaches the
    // speaker. The remaining bits keep the vendor latch value.
    ESP_RETURN_ON_ERROR(WriteRegister(0U, kRegisterOutput, 0b0110'0100U), kTag, "U1 output latch failed");
    ESP_RETURN_ON_ERROR(WriteRegister(0U, kRegisterOutputHighImpedance, 0b0000'0000U), kTag,
                        "U1 high-impedance mask failed");
    ESP_RETURN_ON_ERROR(WriteRegister(0U, kRegisterIoDirection, 0b0111'1111U), kTag, "U1 directions failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    // U2: WLAN power / USB 5 V / charge enables are outputs, power-off pulse
    // latch low, rail defaults from the vendor BSP.
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterIoDirection, 0b1011'1001U), kTag, "U2 directions failed");
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterOutputHighImpedance, 0b0000'0110U), kTag,
                        "U2 high-impedance mask failed");
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterPullSelect, 0b1011'1001U), kTag, "U2 pull select failed");
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterPullEnable, 0b1111'1001U), kTag, "U2 pull enable failed");
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterInputDefault, 0b0100'0000U), kTag, "U2 input default failed");
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterInterruptMask, 0b1011'1111U), kTag, "U2 interrupt mask failed");
    // P0 (WLAN power) and P3 (USB 5 V) high, P5 low (QC enable is active-low),
    // P7 high (charge enable). The vendor expander initialization leaves
    // charging off and its application enables QC and charging right after, so
    // mirror that here to keep the board charging like the official firmware.
    ESP_RETURN_ON_ERROR(WriteRegister(1U, kRegisterOutput, 0b1000'1001U), kTag, "U2 output latch failed");
    return ESP_OK;
}

esp_err_t Pi4ioeExpander::ResetPanelAndTouch() {
    // Vendor sequence for the ILI9881C + GT911 panel: hold both resets low for
    // 100 ms, then drive both high as push-pull outputs. Releasing the shared
    // LCD reset net to a weak pull-up instead leaves the touch controller in
    // reset, which is why this must stay a driven-high pulse.
    constexpr uint8_t kBothResetBits = static_cast<uint8_t>((1U << kLcdResetBit) | (1U << kTouchResetBit));
    ESP_RETURN_ON_ERROR(UpdateOutput(0U, kBothResetBits, 0U), kTag, "assert panel and touch reset failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(UpdateOutput(0U, kBothResetBits, kBothResetBits), kTag, "release panel and touch reset failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t Pi4ioeExpander::SetOutputBit(uint8_t unit, uint8_t bit, bool high) {
    if (bit >= 8U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = static_cast<uint8_t>(1U << bit);
    return UpdateOutput(unit, mask, high ? mask : 0U);
}

esp_err_t Pi4ioeExpander::ReadInputPort(uint8_t unit, uint8_t& value) {
    return ReadRegister(unit, kRegisterInputPort, &value);
}

}  // namespace micropixel::platform::m5stack_tab5
