// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

#include "esp_err.h"
using gpio_num_t = int;
inline constexpr gpio_num_t GPIO_NUM_NC = -1;
inline constexpr gpio_num_t GPIO_NUM_MAX = 55;
inline constexpr gpio_num_t GPIO_NUM_2 = 2;
inline constexpr gpio_num_t GPIO_NUM_3 = 3;
inline constexpr gpio_num_t GPIO_NUM_4 = 4;
inline constexpr gpio_num_t GPIO_NUM_7 = 7;
inline constexpr gpio_num_t GPIO_NUM_8 = 8;
inline constexpr gpio_num_t GPIO_NUM_9 = 9;
inline constexpr gpio_num_t GPIO_NUM_10 = 10;
inline constexpr gpio_num_t GPIO_NUM_12 = 12;
inline constexpr gpio_num_t GPIO_NUM_13 = 13;
inline constexpr gpio_num_t GPIO_NUM_22 = 22;
inline constexpr gpio_num_t GPIO_NUM_26 = 26;
inline constexpr gpio_num_t GPIO_NUM_27 = 27;
inline constexpr gpio_num_t GPIO_NUM_28 = 28;
inline constexpr gpio_num_t GPIO_NUM_29 = 29;
inline constexpr gpio_num_t GPIO_NUM_33 = 33;
inline constexpr gpio_num_t GPIO_NUM_52 = 52;
enum gpio_mode_t { GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT };
enum gpio_pullup_t { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE };
enum gpio_pulldown_t { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE };
enum gpio_int_type_t { GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE };
struct gpio_config_t {
    uint64_t pin_bit_mask{};
    gpio_mode_t mode{};
    gpio_pullup_t pull_up_en{};
    gpio_pulldown_t pull_down_en{};
    gpio_int_type_t intr_type{};
};
struct TestGpioPin {
    int level{};
    bool interrupt_enabled{};
    gpio_mode_t mode{};
    void (*handler)(void*){};
    void* context{};
};
inline std::array<TestGpioPin, GPIO_NUM_MAX> gpio_pins{};
inline int gpio_install_count{};
inline bool gpio_isr_installed{};
inline int gpio_fail_handler_pin{-1};
inline esp_err_t gpio_install_isr_service(int) {
    ++gpio_install_count;
    if (gpio_isr_installed) return ESP_ERR_INVALID_STATE;
    gpio_isr_installed = true;
    return ESP_OK;
}
inline esp_err_t gpio_config(const gpio_config_t* config) {
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin)
        if ((config->pin_bit_mask & (1ULL << pin)) != 0) gpio_pins[pin].mode = config->mode;
    return ESP_OK;
}
inline int gpio_get_level(gpio_num_t pin) { return gpio_pins[pin].level; }
inline esp_err_t gpio_set_level(gpio_num_t pin, uint32_t value) {
    gpio_pins[pin].level = value;
    return ESP_OK;
}
inline esp_err_t gpio_reset_pin(gpio_num_t pin) {
    gpio_pins[pin] = {};
    return ESP_OK;
}
inline esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void*), void* context) {
    if (pin == gpio_fail_handler_pin) return ESP_FAIL;
    gpio_pins[pin].handler = handler;
    gpio_pins[pin].context = context;
    return ESP_OK;
}
inline esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
    gpio_pins[pin].handler = nullptr;
    return ESP_OK;
}
inline esp_err_t gpio_intr_enable(gpio_num_t pin) {
    gpio_pins[pin].interrupt_enabled = true;
    return ESP_OK;
}
inline esp_err_t gpio_intr_disable(gpio_num_t pin) {
    gpio_pins[pin].interrupt_enabled = false;
    return ESP_OK;
}
