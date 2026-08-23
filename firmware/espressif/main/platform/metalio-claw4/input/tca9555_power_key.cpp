// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/metalio-claw4/input/tca9555_power_key.hpp"

#include <cinttypes>

#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "iot_button.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_power_key";
constexpr gpio_num_t kTcaInterrupt = GPIO_NUM_2;
constexpr uint8_t kInputPort0 = 0x00U;
constexpr uint8_t kInputPort1 = 0x01U;
constexpr uint8_t kConfigurationPort0 = 0x06U;
constexpr uint8_t kPowerKeyMask = 1U << 5U;
constexpr int kI2cTimeoutMs = 20;

esp_err_t ReadRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t& value) {
    return i2c_master_transmit_receive(device, &address, sizeof(address), &value, sizeof(value), kI2cTimeoutMs);
}

esp_err_t WriteRegister(i2c_master_dev_handle_t device, uint8_t address, uint8_t value) {
    const uint8_t transaction[] = {address, value};
    return i2c_master_transmit(device, transaction, sizeof(transaction), kI2cTimeoutMs);
}

}  // namespace

esp_err_t Tca9555PowerKey::Initialize(i2c_master_dev_handle_t io_expander) {
    if (io_expander == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (initialized_ || button_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    io_expander_ = io_expander;

    gpio_config_t interrupt_config{};
    interrupt_config.pin_bit_mask = 1ULL << kTcaInterrupt;
    interrupt_config.mode = GPIO_MODE_INPUT;
    interrupt_config.pull_up_en = GPIO_PULLUP_DISABLE;
    interrupt_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    interrupt_config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t status = gpio_config(&interrupt_config);
    if (status != ESP_OK) {
        return status;
    }

    status = ConfigurePowerKeyInput();
    if (status != ESP_OK) {
        return status;
    }
    uint8_t initial_port0 = 0U;
    uint8_t initial_port1 = 0U;
    const int initial_interrupt_level = gpio_get_level(kTcaInterrupt);
    status = ReadPorts(initial_port0, initial_port1);
    if (status != ESP_OK) {
        return status;
    }
    (void)RecordRawLevel(initial_port0, initial_port1, initial_interrupt_level);

    driver_.owner = this;
    driver_.base.enable_power_save = true;
    driver_.base.get_key_level = GetKeyLevel;
    driver_.base.enter_power_save = EnterPowerSaveCallback;
    driver_.base.exit_power_save = ExitPowerSaveCallback;
    driver_.base.get_gpio_num = GetInterruptGpio;
    driver_.base.del = DeleteDriver;

    button_config_t button_config{};
    status = iot_button_create(&button_config, &driver_.base, &button_);
    if (status != ESP_OK) {
        return status;
    }

    constexpr button_event_t kLoggedEvents[] = {
        BUTTON_PRESS_DOWN,   BUTTON_PRESS_UP,         BUTTON_SINGLE_CLICK,
        BUTTON_DOUBLE_CLICK, BUTTON_LONG_PRESS_START, BUTTON_LONG_PRESS_UP,
    };
    for (button_event_t event : kLoggedEvents) {
        status = iot_button_register_cb(button_, event, nullptr, ButtonEvent, this);
        if (status != ESP_OK) {
            (void)iot_button_delete(button_);
            button_ = nullptr;
            return status;
        }
    }

    status = ConfigureInterrupt();
    if (status != ESP_OK) {
        (void)iot_button_delete(button_);
        button_ = nullptr;
        return status;
    }
    initialized_ = true;

    if (last_key_level_ == BUTTON_ACTIVE) {
        status = iot_button_resume();
    } else {
        status = EnterPowerSave();
    }
    if (status != ESP_OK) {
        (void)iot_button_delete(button_);
        button_ = nullptr;
        return status;
    }

    ESP_LOGI(kTag, "ready: TCA9555 P0.5 active-low, TCA_INT GPIO%d low-level wake", kTcaInterrupt);
    return ESP_OK;
}

esp_err_t Tca9555PowerKey::ConfigurePowerKeyInput() {
    uint8_t configuration = 0U;
    esp_err_t status = ReadRegister(io_expander_, kConfigurationPort0, configuration);
    if (status != ESP_OK || (configuration & kPowerKeyMask) != 0U) {
        return status;
    }
    configuration |= kPowerKeyMask;
    return WriteRegister(io_expander_, kConfigurationPort0, configuration);
}

esp_err_t Tca9555PowerKey::ConfigureInterrupt() {
    esp_err_t status = gpio_set_intr_type(kTcaInterrupt, GPIO_INTR_LOW_LEVEL);
    if (status != ESP_OK) {
        return status;
    }
    status = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (status != ESP_OK && status != ESP_ERR_INVALID_STATE) {
        return status;
    }
    status = gpio_isr_handler_add(kTcaInterrupt, InterruptEntry, this);
    if (status != ESP_OK) {
        return status;
    }
    isr_registered_ = true;
    return esp_sleep_enable_gpio_wakeup();
}

esp_err_t Tca9555PowerKey::ReadPorts(uint8_t& port0, uint8_t& port1) const {
    esp_err_t status = ReadRegister(io_expander_, kInputPort0, port0);
    if (status != ESP_OK) {
        return status;
    }
    return ReadRegister(io_expander_, kInputPort1, port1);
}

uint8_t Tca9555PowerKey::ReadKeyLevel() {
    uint8_t port0 = 0U;
    uint8_t port1 = 0U;
    const int interrupt_level = gpio_get_level(kTcaInterrupt);
    esp_err_t status = ReadPorts(port0, port1);
    if (status != ESP_OK) {
        ++read_error_count_;
        if (read_error_count_ == 1U || (read_error_count_ % 100U) == 0U) {
            ESP_LOGW(kTag, "TCA9555 input read failed (#%" PRIu32 "): %s", read_error_count_, esp_err_to_name(status));
        }
        return last_key_level_;
    }
    if (read_error_count_ != 0U) {
        ESP_LOGI(kTag, "TCA9555 input reads recovered after %" PRIu32 " failures", read_error_count_);
        read_error_count_ = 0U;
    }
    return RecordRawLevel(port0, port1, interrupt_level);
}

uint8_t Tca9555PowerKey::RecordRawLevel(uint8_t port0, uint8_t port1, int interrupt_level) {
    const uint8_t key_level = (port0 & kPowerKeyMask) == 0U ? BUTTON_ACTIVE : BUTTON_INACTIVE;
    if (!raw_level_known_ || key_level != last_key_level_) {
        ESP_LOGI(kTag, "raw %s: P0=0x%02x P1=0x%02x TCA_INT(before-read)=%d",
                 key_level == BUTTON_ACTIVE ? "down" : "up", port0, port1, interrupt_level);
    }
    raw_level_known_ = true;
    last_key_level_ = key_level;
    return key_level;
}

esp_err_t Tca9555PowerKey::EnterPowerSave() {
    esp_err_t status = gpio_wakeup_enable(kTcaInterrupt, GPIO_INTR_LOW_LEVEL);
    return status == ESP_OK ? gpio_intr_enable(kTcaInterrupt) : status;
}

esp_err_t Tca9555PowerKey::ExitPowerSave() {
    esp_err_t status = gpio_intr_disable(kTcaInterrupt);
    esp_err_t wake_status = gpio_wakeup_disable(kTcaInterrupt);
    return status == ESP_OK ? wake_status : status;
}

uint8_t Tca9555PowerKey::GetKeyLevel(button_driver_t* driver) {
    auto* context = reinterpret_cast<DriverContext*>(driver);
    return context->owner->ReadKeyLevel();
}

esp_err_t Tca9555PowerKey::EnterPowerSaveCallback(button_driver_t* driver) {
    auto* context = reinterpret_cast<DriverContext*>(driver);
    return context->owner->EnterPowerSave();
}

esp_err_t Tca9555PowerKey::ExitPowerSaveCallback(button_driver_t* driver) {
    auto* context = reinterpret_cast<DriverContext*>(driver);
    return context->owner->ExitPowerSave();
}

int32_t Tca9555PowerKey::GetInterruptGpio(button_driver_t*) { return kTcaInterrupt; }

esp_err_t Tca9555PowerKey::DeleteDriver(button_driver_t* driver) {
    auto* context = reinterpret_cast<DriverContext*>(driver);
    Tca9555PowerKey* owner = context->owner;
    (void)owner->ExitPowerSave();
    if (owner->isr_registered_) {
        (void)gpio_isr_handler_remove(kTcaInterrupt);
        owner->isr_registered_ = false;
    }
    owner->button_ = nullptr;
    owner->initialized_ = false;
    return ESP_OK;
}

void IRAM_ATTR Tca9555PowerKey::InterruptEntry(void*) {
    iot_button_power_save_wakeup_isr(static_cast<uint32_t>(kTcaInterrupt));
}

void Tca9555PowerKey::ButtonEvent(void* button, void*) {
    auto button_handle = static_cast<button_handle_t>(button);
    button_event_t event = iot_button_get_event(button_handle);
    if (event == BUTTON_PRESS_UP || event == BUTTON_LONG_PRESS_UP) {
        ESP_LOGI(kTag, "event %s, pressed=%" PRIu32 " ms", iot_button_get_event_str(event),
                 iot_button_get_pressed_time(button_handle));
        return;
    }
    ESP_LOGI(kTag, "event %s", iot_button_get_event_str(event));
}

}  // namespace micropixel::platform::metalio_claw4
