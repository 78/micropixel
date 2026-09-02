#include "platform/boards/m5stack-cores3/board_hardware.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::m5stack_cores3 {
namespace {

constexpr char kTag[] = "cores3_hw";
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kI2cSda = GPIO_NUM_12;
constexpr gpio_num_t kI2cScl = GPIO_NUM_11;
constexpr uint16_t kAw9523Address = 0x58U;
constexpr uint8_t kTouchEnablePin = 0U;
constexpr uint8_t kSpeakerEnablePin = 2U;
constexpr uint8_t kLcdEnablePin = 9U;
constexpr uint8_t kTouchInterruptPin = 10U;
constexpr uint8_t kSpeakerInterruptPin = 11U;
constexpr uint8_t kAxpLdoEnableSpeaker = 1U << 1U;
constexpr uint8_t kAxpLdoEnableBacklight = 1U << 7U;
constexpr uint8_t kAxpSpeakerVoltageRegister = 0x92U;
constexpr uint8_t kAxpBacklightVoltageRegister = 0x99U;
constexpr int kI2cTimeoutMs = 100;
constexpr uint32_t kAudioPowerSettleMs = 10U;
constexpr uint32_t kExpanderReadyDelayMs = 20U;
constexpr uint32_t kExpanderReadyAttempts = 5U;
constexpr uint32_t kTouchStartupMs = 300U;
constexpr uint32_t kTouchPowerOffMs = 500U;

esp_err_t ReadRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t& value) {
    return i2c_master_transmit_receive(device, &reg, sizeof(reg), &value, sizeof(value), kI2cTimeoutMs);
}

esp_err_t WriteRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    const uint8_t bytes[]{reg, value};
    return i2c_master_transmit(device, bytes, sizeof(bytes), kI2cTimeoutMs);
}

esp_err_t WaitForExpander(i2c_master_bus_handle_t bus) {
    esp_err_t status = ESP_ERR_NOT_FOUND;
    for (uint32_t attempt = 0U; attempt < kExpanderReadyAttempts; ++attempt) {
        status = i2c_master_probe(bus, kAw9523Address, kI2cTimeoutMs);
        if (status == ESP_OK) {
            return ESP_OK;
        }
        // CoreS3 keeps several peripherals powered across an ESP-only reset.
        // Recover the controller and allow the expander to release the bus
        // before treating a transient missing ACK as a board failure.
        (void)i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(kExpanderReadyDelayMs));
    }
    ESP_LOGE(kTag, "AW9523 did not become ready (SCL=%d, SDA=%d): %s", gpio_get_level(kI2cScl), gpio_get_level(kI2cSda),
             esp_err_to_name(status));
    return status;
}

}  // namespace

esp_err_t BoardHardware::Initialize() {
    if (i2c_bus_ != nullptr || expander_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = kI2cPort;
    bus_config.sda_io_num = kI2cSda;
    bus_config.scl_io_num = kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7U;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_), kTag, "create shared I2C bus failed");

    ESP_RETURN_ON_ERROR(pmic_.Attach(i2c_bus_), kTag, "attach AXP2101 failed");
    ESP_RETURN_ON_ERROR(WaitForExpander(i2c_bus_), kTag, "wait for AW9523 failed");
    i2c_device_config_t expander_config{};
    expander_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    expander_config.device_address = kAw9523Address;
    expander_config.scl_speed_hz = 400000U;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_, &expander_config, &expander_), kTag,
                        "attach AW9523 failed");
    // Do not soft-reset the board-wide expander or configure all 16 pins as
    // outputs. Several unowned pins are interrupt inputs, and the power/bus
    // direction pins deliberately retain state across an ESP-only reset.
    // Normalize only the feature-control outputs and interrupt inputs this
    // profile owns.
    // In particular, leave BUS_OUT_EN/BOOST_EN untouched: external 5 V output
    // is a separate power-policy capability and must account for VBUS and
    // battery state before enabling the boost converter.
    uint8_t control = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(expander_, 0x11U, control), kTag, "read AW9523 control failed");
    ESP_RETURN_ON_ERROR(WriteRegister(expander_, 0x11U, static_cast<uint8_t>(control | 0x10U)), kTag,
                        "configure AW9523 port 0 failed");
    ESP_RETURN_ON_ERROR(SetExpanderPin(kSpeakerEnablePin, false), kTag, "hold AW88298 in reset failed");
    ESP_RETURN_ON_ERROR(SetExpanderInput(kTouchInterruptPin), kTag, "configure touch interrupt failed");
    return SetExpanderInput(kSpeakerInterruptPin);
}

esp_err_t BoardHardware::EnableDisplayAndTouch() {
    if (executor_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // These expander pins gate peripheral supplies; they are not reset inputs.
    // An ESP-only reset leaves both peripherals powered, so keep their rails
    // enabled instead of applying a short and incomplete power cycle.
    ESP_RETURN_ON_ERROR(SetExpanderPin(kLcdEnablePin, true), kTag, "enable LCD failed");
    ESP_RETURN_ON_ERROR(SetExpanderPin(kTouchEnablePin, true), kTag, "enable touch failed");
    vTaskDelay(pdMS_TO_TICKS(kTouchStartupMs));
    return ESP_OK;
}

esp_err_t BoardHardware::RecoverTouch() {
    if (executor_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(kTag, "power-cycling FT6336U after failed probe");
    ESP_RETURN_ON_ERROR(SetExpanderPin(kTouchEnablePin, false), kTag, "disable touch failed");
    vTaskDelay(pdMS_TO_TICKS(kTouchPowerOffMs));
    ESP_RETURN_ON_ERROR(SetExpanderPin(kTouchEnablePin, true), kTag, "re-enable touch failed");
    vTaskDelay(pdMS_TO_TICKS(kTouchStartupMs));
    return i2c_master_bus_reset(i2c_bus_);
}

esp_err_t BoardHardware::SetExpanderPin(uint8_t pin, bool enabled) {
    if (expander_ == nullptr || pin >= 16U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t bank = pin / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (pin % 8U));
    const uint8_t output_register = static_cast<uint8_t>(0x02U + bank);
    const uint8_t direction_register = static_cast<uint8_t>(0x04U + bank);
    const uint8_t mode_register = static_cast<uint8_t>(0x12U + bank);
    uint8_t output = 0U;
    uint8_t direction = 0U;
    uint8_t mode = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(expander_, output_register, output), kTag, "read AW9523 output failed");
    ESP_RETURN_ON_ERROR(ReadRegister(expander_, direction_register, direction), kTag, "read AW9523 direction failed");
    ESP_RETURN_ON_ERROR(ReadRegister(expander_, mode_register, mode), kTag, "read AW9523 mode failed");
    const uint8_t next_output = enabled ? static_cast<uint8_t>(output | mask) : static_cast<uint8_t>(output & ~mask);
    ESP_RETURN_ON_ERROR(WriteRegister(expander_, output_register, next_output), kTag, "write AW9523 output failed");
    // AW9523 retains state while an ESP-only reset leaves the board rails up.
    // A high mode bit selects GPIO rather than LED-current mode. Normalize the
    // owned pin before making it an output so reset pulses reach the peripheral.
    const uint8_t next_mode = static_cast<uint8_t>(mode | mask);
    if (next_mode != mode) {
        ESP_RETURN_ON_ERROR(WriteRegister(expander_, mode_register, next_mode), kTag, "write AW9523 mode failed");
    }
    const uint8_t next_direction = static_cast<uint8_t>(direction & ~mask);
    return next_direction == direction ? ESP_OK : WriteRegister(expander_, direction_register, next_direction);
}

esp_err_t BoardHardware::SetExpanderInput(uint8_t pin) {
    if (expander_ == nullptr || pin >= 16U) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t bank = pin / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (pin % 8U));
    const uint8_t direction_register = static_cast<uint8_t>(0x04U + bank);
    uint8_t direction = 0U;
    ESP_RETURN_ON_ERROR(ReadRegister(expander_, direction_register, direction), kTag, "read AW9523 direction failed");
    const uint8_t next_direction = static_cast<uint8_t>(direction | mask);
    return next_direction == direction ? ESP_OK : WriteRegister(expander_, direction_register, next_direction);
}

esp_err_t BoardHardware::SetBrightness(int percent) {
    if (executor_ == nullptr) {
        return SetBrightnessOnBus(percent);
    }
    struct Request final {
        BoardHardware* hardware;
        int percent;
    } request{this, percent};
    return executor_->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) {
            auto& requested = *static_cast<Request*>(context);
            return requested.hardware->SetBrightnessOnBus(requested.percent);
        },
        &request);
}

esp_err_t BoardHardware::SetBrightnessOnBus(int percent) {
    const int bounded = std::clamp(percent, 0, 100);
    const int voltage_mv = bounded == 0 ? 0 : 2500 + (bounded * 800) / 100;
    return pmic_.SetLdoVoltage(kAxpBacklightVoltageRegister, kAxpLdoEnableBacklight, voltage_mv);
}

esp_err_t BoardHardware::PowerOnAudio() {
    if (executor_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t status = executor_->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) { return static_cast<BoardHardware*>(context)->SetAudioPowerOnBus(true); }, this);
    if (status == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(kAudioPowerSettleMs));
    }
    return status;
}

esp_err_t BoardHardware::PowerOffAudio() {
    if (executor_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return executor_->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) { return static_cast<BoardHardware*>(context)->SetAudioPowerOnBus(false); }, this);
}

esp_err_t BoardHardware::SetAudioPowerOnBus(bool enabled) {
    if (enabled == audio_powered_) {
        return ESP_OK;
    }
    if (enabled) {
        ESP_RETURN_ON_ERROR(SetExpanderPin(kSpeakerEnablePin, true), kTag, "enable speaker gate failed");
        const esp_err_t status = pmic_.SetLdoVoltage(kAxpSpeakerVoltageRegister, kAxpLdoEnableSpeaker, 1800);
        if (status != ESP_OK) {
            (void)SetExpanderPin(kSpeakerEnablePin, false);
            return status;
        }
    } else {
        ESP_RETURN_ON_ERROR(SetExpanderPin(kSpeakerEnablePin, false), kTag, "disable speaker gate failed");
        ESP_RETURN_ON_ERROR(pmic_.SetLdoVoltage(kAxpSpeakerVoltageRegister, kAxpLdoEnableSpeaker, 0), kTag,
                            "disable speaker rail failed");
    }
    audio_powered_ = enabled;
    return ESP_OK;
}

esp_err_t BoardHardware::ReadBatteryOnBus(drivers::Axp2101BatterySample& sample) { return pmic_.ReadBattery(sample); }

esp_err_t BoardHardware::RequestPowerOff() {
    if (executor_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return executor_->Invoke(
        buses::I2cExecutor::Priority::kHigh,
        [](void* context) { return static_cast<BoardHardware*>(context)->RequestPowerOffOnBus(); }, this);
}

esp_err_t BoardHardware::RequestPowerOffOnBus() {
    (void)SetAudioPowerOnBus(false);
    (void)SetBrightnessOnBus(0);
    return pmic_.PowerOff();
}

}  // namespace micropixel::platform::m5stack_cores3
