// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

#include "esp_err.h"
enum ledc_mode_t { LEDC_LOW_SPEED_MODE };
enum ledc_timer_t { LEDC_TIMER_0, LEDC_TIMER_1, LEDC_TIMER_2, LEDC_TIMER_3 };
enum ledc_channel_t { LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3 };
enum ledc_timer_bit_t { LEDC_TIMER_10_BIT = 10 };
inline constexpr int LEDC_AUTO_CLK = 0;
struct ledc_timer_config_t {
    ledc_mode_t speed_mode{};
    ledc_timer_bit_t duty_resolution{};
    ledc_timer_t timer_num{};
    uint32_t freq_hz{};
    int clk_cfg{};
};
struct ledc_channel_config_t {
    int gpio_num{};
    ledc_mode_t speed_mode{};
    ledc_channel_t channel{};
    ledc_timer_t timer_sel{};
    uint32_t duty{};
};
inline std::array<ledc_channel_config_t, 4> gpio_pwm_channels{};
inline std::array<bool, 4> gpio_pwm_active{};
inline esp_err_t ledc_timer_config(const ledc_timer_config_t*) { return ESP_OK; }
inline esp_err_t ledc_channel_config(const ledc_channel_config_t* config) {
    gpio_pwm_channels[config->channel] = *config;
    gpio_pwm_active[config->channel] = true;
    return ESP_OK;
}
inline esp_err_t ledc_set_duty(ledc_mode_t, ledc_channel_t channel, uint32_t duty) {
    gpio_pwm_channels[channel].duty = duty;
    return ESP_OK;
}
inline esp_err_t ledc_update_duty(ledc_mode_t, ledc_channel_t) { return ESP_OK; }
inline esp_err_t ledc_stop(ledc_mode_t, ledc_channel_t channel, uint32_t) {
    gpio_pwm_active[channel] = false;
    return ESP_OK;
}
