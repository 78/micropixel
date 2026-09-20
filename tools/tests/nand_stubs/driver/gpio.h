// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "esp_err.h"
using gpio_num_t = int;
constexpr int GPIO_NUM_NC = -1;
constexpr int GPIO_MODE_OUTPUT = 1, GPIO_PULLUP_ENABLE = 1, GPIO_PULLDOWN_DISABLE = 0, GPIO_INTR_DISABLE = 0;
struct gpio_config_t {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
};
inline esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }
inline esp_err_t gpio_set_level(gpio_num_t, int) { return ESP_OK; }
