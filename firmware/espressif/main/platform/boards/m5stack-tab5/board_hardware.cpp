// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/m5stack-tab5/board_hardware.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/m5stack-tab5/board_config.hpp"

namespace micropixel::platform::m5stack_tab5 {
namespace {

constexpr char kTag[] = "tab5_hw";

}  // namespace

esp_err_t BoardHardware::Initialize() {
    if (i2c_bus_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = board::kSystemI2cPort;
    bus_config.sda_io_num = board::kSystemI2cSda;
    bus_config.scl_io_num = board::kSystemI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7U;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_), kTag, "create system I2C bus failed");

    ESP_RETURN_ON_ERROR(expander_.Initialize(i2c_bus_), kTag, "initialize PI4IOE expanders failed");

    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.timer_num = board::kBacklightTimer;
    timer_config.freq_hz = 5000U;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag, "configure backlight timer failed");

    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = board::kDisplayBacklight;
    channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_config.channel = board::kBacklightChannel;
    channel_config.timer_sel = board::kBacklightTimer;
    channel_config.duty = 0U;
    channel_config.hpoint = 0;
    channel_config.flags.output_invert = false;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), kTag, "configure backlight channel failed");
    brightness_ready_ = true;
    return ESP_OK;
}

esp_err_t BoardHardware::ResetPanelAndTouch() {
    // The controller samples its I2C address from the interrupt pin level while
    // it is being reset, and a high level (the board has an external 3.3 V
    // pull-up) selects 0x14, which is the address the touch driver probes for.
    // Release the pin before pulsing the resets so nothing can hold it low
    // during that latch window.
    gpio_reset_pin(board::kTouchInterrupt);
    ESP_RETURN_ON_ERROR(expander_.ResetPanelAndTouch(), kTag, "pulse panel and touch reset failed");

    // Unlock the touch subsystem. On this panel variant the controller reports
    // nothing at all while the interrupt line idles high through that pull-up
    // - I2C still answers, so the controller looks healthy - and the host has
    // to pull the line low once after a reset to get touch data (esp-bsp,
    // bsp_display.c, panel "fix (ver 1)": "there is resistor to 3V3 on
    // interrupt pin which is blocking GT911 touch").
    // The line goes back to being an interrupt input afterwards; the unlocked
    // state persists until the next reset, which is why this step lives inside
    // the reset helper instead of at the call site.
    gpio_config_t unlock_config{};
    unlock_config.pin_bit_mask = 1ULL << board::kTouchInterrupt;
    unlock_config.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&unlock_config), kTag, "configure touch interrupt output failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(board::kTouchInterrupt, 0), kTag, "drive touch interrupt low failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_config_t interrupt_config{};
    interrupt_config.pin_bit_mask = 1ULL << board::kTouchInterrupt;
    interrupt_config.mode = GPIO_MODE_INPUT;
    interrupt_config.pull_up_en = GPIO_PULLUP_ENABLE;
    interrupt_config.intr_type = GPIO_INTR_NEGEDGE;
    return gpio_config(&interrupt_config);
}

esp_err_t BoardHardware::SetBrightness(int percent) {
    if (!brightness_ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t bounded = static_cast<uint32_t>(std::clamp(percent, 0, 100));
    const uint32_t duty = (board::kBacklightMaximumDuty * bounded + 50U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, board::kBacklightChannel, duty), kTag,
                        "set backlight duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, board::kBacklightChannel);
}

}  // namespace micropixel::platform::m5stack_tab5
