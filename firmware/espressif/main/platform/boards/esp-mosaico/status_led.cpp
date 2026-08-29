#include "platform/boards/esp-mosaico/status_led.hpp"

#include "driver/gpio.h"
#include "esp_check.h"
#include "platform/boards/esp-mosaico/board_config.hpp"

namespace micropixel::platform::esp_mosaico {

esp_err_t StatusLed::Initialize() {
    if (initialized_) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(gpio_set_level(board::kStatusLed, 1), "mosaico_led", "preset status LED failed");
    gpio_config_t config{};
    config.pin_bit_mask = BIT64(board::kStatusLed);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), "mosaico_led", "configure status LED failed");
    initialized_ = true;
    return ESP_OK;
}

esp_err_t StatusLed::Set(bool enabled) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t status = gpio_set_level(board::kStatusLed, enabled ? 0 : 1);
    if (status == ESP_OK) {
        enabled_ = enabled;
    }
    return status;
}

int32_t StatusLed::GetInfo(device::PeripheralChannelId channel, micropixel_gpio_info_t& info_out) const {
    if (channel != kChannel) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    info_out = {};
    info_out.size = sizeof(info_out);
    info_out.line_number = static_cast<uint16_t>(board::kStatusLed);
    info_out.capabilities = MICROPIXEL_GPIO_CAP_OUTPUT;
    return MICROPIXEL_STATUS_OK;
}

int32_t StatusLed::Open(device::PeripheralChannelId channel, uint16_t mode, uint16_t pull, uint16_t edge,
                        uint32_t initial_value, uint32_t pwm_frequency_hz, device::GpioPeripheralEdgeSink, void*) {
    if (channel != kChannel) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (!initialized_ || active_) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    if (mode != MICROPIXEL_GPIO_MODE_OUTPUT || pull != MICROPIXEL_GPIO_PULL_NONE || edge != MICROPIXEL_GPIO_EDGE_NONE ||
        initial_value > 1U || pwm_frequency_hz != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (Set(initial_value != 0U) != ESP_OK) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    active_ = true;
    return MICROPIXEL_STATUS_OK;
}

int32_t StatusLed::Read(device::PeripheralChannelId channel, bool& value_out) const {
    if (channel != kChannel) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (!active_) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    value_out = enabled_;
    return MICROPIXEL_STATUS_OK;
}

int32_t StatusLed::Write(device::PeripheralChannelId channel, bool value) {
    if (channel != kChannel) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (!active_) {
        return MICROPIXEL_STATUS_CLOSED;
    }
    return Set(value) == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t StatusLed::SetPwmDuty(device::PeripheralChannelId channel, uint16_t) {
    return channel == kChannel ? MICROPIXEL_STATUS_UNSUPPORTED : MICROPIXEL_STATUS_NOT_FOUND;
}

void StatusLed::Close(device::PeripheralChannelId channel) {
    if (channel != kChannel || !active_) {
        return;
    }
    (void)Set(false);
    active_ = false;
}

}  // namespace micropixel::platform::esp_mosaico
