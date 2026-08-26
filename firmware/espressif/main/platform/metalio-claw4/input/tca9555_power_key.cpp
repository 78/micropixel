// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/metalio-claw4/input/tca9555_power_key.hpp"

#include <cinttypes>

#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "iot_button.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr char kTag[] = "micropixel_power_key";
constexpr gpio_num_t kTcaInterrupt = GPIO_NUM_2;
constexpr uint8_t kInputPort0 = 0x00U;
constexpr uint8_t kInputPort1 = 0x01U;
constexpr uint8_t kOutputPort0 = 0x02U;
constexpr uint8_t kConfigurationPort0 = 0x06U;
constexpr uint8_t kPowerKeyMask = 1U << 5U;
constexpr uint8_t kPowerKeyPulseMask = 1U << 4U;
constexpr uint8_t kPowerInputMask = (1U << 2U) | (1U << 3U);
constexpr int kI2cTimeoutMs = 20;
constexpr uint64_t kPowerClickGuardUs = 2000000U;
constexpr uint16_t kPowerOffLongPressMs = 2000U;
constexpr TickType_t kPowerOffPulseHalfPeriod = pdMS_TO_TICKS(100U);
constexpr uint32_t kWakeLineDrainAttempts = 4U;
constexpr TickType_t kWakeLineSettleDelay = 1U;

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
    button_config.long_press_time = kPowerOffLongPressMs;
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

void Tca9555PowerKey::SetPowerInputChangeSink(PowerInputChangeSink sink, void* context) {
    power_input_change_context_ = context;
    power_input_change_sink_ = sink;
}

void Tca9555PowerKey::SetKeyPressSink(KeyPressSink sink, void* context) {
    portENTER_CRITICAL(&key_press_sink_lock_);
    key_press_sink_ = sink;
    key_press_context_ = context;
    portEXIT_CRITICAL(&key_press_sink_lock_);
}

void Tca9555PowerKey::SetPowerOffSink(PowerOffSink sink, void* context) {
    portENTER_CRITICAL(&key_press_sink_lock_);
    power_off_sink_ = sink;
    power_off_context_ = context;
    portEXIT_CRITICAL(&key_press_sink_lock_);
}

esp_err_t Tca9555PowerKey::PrepareForLightSleep() {
    esp_err_t status = ExitPowerSave();
    if (status != ESP_OK) {
        return status;
    }

    for (uint32_t attempt = 0U; attempt < kWakeLineDrainAttempts; ++attempt) {
        uint8_t port0 = 0U;
        uint8_t port1 = 0U;
        status = ReadPorts(port0, port1);
        if (status != ESP_OK) {
            (void)EnterPowerSave();
            return status;
        }
        if ((port0 & kPowerKeyMask) == 0U) {
            (void)EnterPowerSave();
            return ESP_ERR_INVALID_STATE;
        }

        if (gpio_get_level(kTcaInterrupt) != 0) {
            status = esp_sleep_enable_gpio_wakeup();
            if (status == ESP_OK) {
                status = EnterPowerSave();
            }
            if (status != ESP_OK) {
                return status;
            }
            if (gpio_get_level(kTcaInterrupt) != 0) {
                return ESP_OK;
            }
            (void)ExitPowerSave();
        }

        if (attempt + 1U < kWakeLineDrainAttempts) {
            vTaskDelay(kWakeLineSettleDelay);
        }
    }

    ESP_LOGW(kTag, "TCA9555 interrupt remained asserted while arming light-sleep wake");
    (void)EnterPowerSave();
    return ESP_ERR_TIMEOUT;
}

bool Tca9555PowerKey::IsPressed() {
    uint8_t port0 = 0U;
    uint8_t port1 = 0U;
    return ReadPorts(port0, port1) == ESP_OK && (port0 & kPowerKeyMask) == 0U;
}

bool Tca9555PowerKey::PowerPressOccurredAfterRequest() const {
    return press_generation_.load(std::memory_order_acquire) !=
           power_request_generation_.load(std::memory_order_acquire);
}

void Tca9555PowerKey::GuardWakeButtonUntilRelease(uint64_t click_deadline_us) {
    wake_key_release_required_.store(true, std::memory_order_release);
    SuppressSingleClicksUntil(click_deadline_us);

    uint8_t port0 = 0U;
    uint8_t port1 = 0U;
    const esp_err_t status = ReadPorts(port0, port1);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "could not confirm wake-key state; power requests remain blocked: %s", esp_err_to_name(status));
        return;
    }
    if ((port0 & kPowerKeyMask) != 0U) {
        wake_key_release_required_.store(false, std::memory_order_release);
        power_request_generation_.store(press_generation_.load(std::memory_order_acquire), std::memory_order_release);
        ESP_LOGI(kTag, "wake key already released; guarded clicks remain suppressed");
        return;
    }
    ESP_LOGI(kTag, "wake key remains pressed; power requests are blocked until release");
}

void Tca9555PowerKey::SuppressSingleClicksUntil(uint64_t deadline_us) {
    uint64_t current = suppress_single_click_until_us_.load(std::memory_order_acquire);
    while (current < deadline_us &&
           !suppress_single_click_until_us_.compare_exchange_weak(current, deadline_us, std::memory_order_acq_rel)) {
    }
}

[[noreturn]] void Tca9555PowerKey::PowerOff() {
    uint8_t output = 0U;
    uint8_t configuration = 0U;
    for (;;) {
        esp_err_t status = ReadRegister(io_expander_, kOutputPort0, output);
        if (status == ESP_OK) {
            output = static_cast<uint8_t>(output & ~kPowerKeyPulseMask);
            status = WriteRegister(io_expander_, kOutputPort0, output);
        }
        if (status == ESP_OK) {
            status = ReadRegister(io_expander_, kConfigurationPort0, configuration);
        }
        if (status == ESP_OK) {
            configuration = static_cast<uint8_t>(configuration & ~kPowerKeyPulseMask);
            status = WriteRegister(io_expander_, kConfigurationPort0, configuration);
        }
        if (status == ESP_OK) {
            break;
        }
        ESP_LOGE(kTag, "power-off pulse configuration failed; retrying: %s", esp_err_to_name(status));
        vTaskDelay(kPowerOffPulseHalfPeriod);
    }

    ESP_LOGI(kTag, "pulsing TCA9555 P0.4 until the power-management IC cuts power");
    for (;;) {
        const esp_err_t high_status = WriteRegister(io_expander_, kOutputPort0, output | kPowerKeyPulseMask);
        if (high_status != ESP_OK) {
            ESP_LOGE(kTag, "power-off pulse high failed: %s", esp_err_to_name(high_status));
        }
        vTaskDelay(kPowerOffPulseHalfPeriod);
        const esp_err_t low_status = WriteRegister(io_expander_, kOutputPort0, output);
        if (low_status != ESP_OK) {
            ESP_LOGE(kTag, "power-off pulse low failed: %s", esp_err_to_name(low_status));
        }
        vTaskDelay(kPowerOffPulseHalfPeriod);
    }
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
    const uint8_t power_inputs = port1 & kPowerInputMask;
    if (!raw_level_known_ || key_level != last_key_level_) {
        ESP_LOGI(kTag, "raw %s: P0=0x%02x P1=0x%02x TCA_INT(before-read)=%d",
                 key_level == BUTTON_ACTIVE ? "down" : "up", port0, port1, interrupt_level);
    }
    raw_level_known_ = true;
    last_key_level_ = key_level;
    if (key_level == BUTTON_INACTIVE && wake_key_release_required_.exchange(false, std::memory_order_acq_rel)) {
        power_request_generation_.store(press_generation_.load(std::memory_order_acquire), std::memory_order_release);
        ESP_LOGI(kTag, "wake key release observed at raw input; power requests rearmed");
    }
    const bool power_inputs_changed = power_inputs_known_ && power_inputs != last_power_inputs_;
    power_inputs_known_ = true;
    last_power_inputs_ = power_inputs;
    if (power_inputs_changed && power_input_change_sink_ != nullptr) {
        power_input_change_sink_(power_input_change_context_);
    }
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

void Tca9555PowerKey::ButtonEvent(void* button, void* context) {
    auto* owner = static_cast<Tca9555PowerKey*>(context);
    auto button_handle = static_cast<button_handle_t>(button);
    button_event_t event = iot_button_get_event(button_handle);
    if (event == BUTTON_PRESS_UP || event == BUTTON_LONG_PRESS_UP) {
        ESP_LOGI(kTag, "event %s, pressed=%" PRIu32 " ms", iot_button_get_event_str(event),
                 iot_button_get_pressed_time(button_handle));
        if (owner != nullptr && owner->wake_key_release_required_.exchange(false, std::memory_order_acq_rel)) {
            // This press was consumed as the wake source. Make it the new
            // baseline so a later automatic-sleep request does not mistake it
            // for a press that happened during that new transition.
            owner->power_request_generation_.store(owner->press_generation_.load(std::memory_order_acquire),
                                                   std::memory_order_release);
            ESP_LOGI(kTag, "wake key released; power requests rearmed");
        }
        return;
    }
    ESP_LOGI(kTag, "event %s", iot_button_get_event_str(event));
    if (event == BUTTON_PRESS_DOWN && owner != nullptr) {
        owner->press_generation_.fetch_add(1U, std::memory_order_acq_rel);
    }
    if (event != BUTTON_SINGLE_CLICK || owner == nullptr) {
        if (event == BUTTON_LONG_PRESS_START && owner != nullptr) {
            if (owner->wake_key_release_required_.load(std::memory_order_acquire)) {
                ESP_LOGI(kTag, "suppressing power-off hold until the wake key is released");
                return;
            }
            PowerOffSink sink = nullptr;
            void* sink_context = nullptr;
            portENTER_CRITICAL(&owner->key_press_sink_lock_);
            sink = owner->power_off_sink_;
            sink_context = owner->power_off_context_;
            portEXIT_CRITICAL(&owner->key_press_sink_lock_);
            if (sink == nullptr || !sink(sink_context, static_cast<uint64_t>(esp_timer_get_time()))) {
                ESP_LOGI(kTag, "power-off request rejected by Host power supervisor");
            } else {
                ESP_LOGI(kTag, "two-second power-button hold accepted as a power-off request");
            }
        }
        return;
    }
    if (owner->wake_key_release_required_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "suppressing power-button click until the wake key is released");
        return;
    }

    const uint64_t timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t suppress_until_us = owner->suppress_single_click_until_us_.load(std::memory_order_acquire);
    if (timestamp_us <= suppress_until_us) {
        ESP_LOGI(kTag, "suppressing guarded power-button click");
        return;
    }

    KeyPressSink sink = nullptr;
    void* sink_context = nullptr;
    portENTER_CRITICAL(&owner->key_press_sink_lock_);
    sink = owner->key_press_sink_;
    sink_context = owner->key_press_context_;
    portEXIT_CRITICAL(&owner->key_press_sink_lock_);
    if (sink == nullptr || !sink(sink_context, timestamp_us)) {
        ESP_LOGI(kTag, "power-button request rejected by Host power supervisor");
        owner->SuppressSingleClicksUntil(timestamp_us + kPowerClickGuardUs);
        return;
    }

    owner->power_request_generation_.store(owner->press_generation_.load(std::memory_order_acquire),
                                           std::memory_order_release);
    owner->SuppressSingleClicksUntil(timestamp_us + kPowerClickGuardUs);
}

}  // namespace micropixel::platform::metalio_claw4
