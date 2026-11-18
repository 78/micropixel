// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/reterminal-d1001/board_hardware.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_io_expander_pca9535.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/reterminal-d1001/board_config.hpp"

namespace micropixel::platform::reterminal_d1001 {
namespace {

constexpr char kTag[] = "d1001_hw";

}  // namespace

esp_err_t BoardHardware::SetExpanderLevel(uint32_t mask, bool high) {
    if (expander_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_io_expander_set_level(expander_, mask, high ? 1 : 0);
}

esp_err_t BoardHardware::ConfigureRails() {
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander_, board::kExpanderOutputMask, IO_EXPANDER_OUTPUT), kTag,
                        "configure PCA9535 outputs failed");

    // Vendor power-up order: latch the 3V3 rail first, then the display rail and
    // backlight enable, then release both resets.
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderPowerHold, true), kTag, "latch 3V3 rail failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderBacklightEnable, true), kTag, "enable backlight rail failed");
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderLcdPowerEnable, true), kTag, "enable LCD power failed");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderLcdReset, true), kTag, "release LCD reset failed");
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderTouchReset, true), kTag, "release touch reset failed");

    // Battery sampling and charging are safe defaults; the amplifier stays off
    // until the audio sink enables it.
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderBatteryReadEnable, true), kTag,
                        "enable battery sampling failed");
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderChargeEnable, false), kTag, "enable charging failed");
    return SetExpanderLevel(board::kExpanderSpeakerAmpEnable, false);
}

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

    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_pca9535(i2c_bus_, ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_000, &expander_),
                        kTag, "create PCA9535 expander failed");
    ESP_RETURN_ON_ERROR(ConfigureRails(), kTag, "configure board rails failed");

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
    if (expander_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Vendor sequence: 1 -> 0 (5/10 ms) -> 1 (120 ms). The panel does not
    // expose a reset GPIO of its own; esp_lcd_new_panel_jd9365 must be created
    // with reset_gpio_num = -1 so the driver never touches a real pin.
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderLcdReset, true), kTag, "raise LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderLcdReset, false), kTag, "assert LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderLcdReset, true), kTag, "release LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    // Give the touch controller a clean reset too. Its driver pulses the same
    // expander line again during initialization.
    ESP_RETURN_ON_ERROR(SetExpanderLevel(board::kExpanderTouchReset, false), kTag, "assert touch reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return SetExpanderLevel(board::kExpanderTouchReset, true);
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

esp_err_t BoardHardware::SetTouchReset(bool asserted) {
    return SetExpanderLevel(board::kExpanderTouchReset, !asserted);
}

esp_err_t BoardHardware::SetSpeakerAmplifier(bool enabled) {
    return SetExpanderLevel(board::kExpanderSpeakerAmpEnable, enabled);
}

esp_err_t BoardHardware::SetPowerHold(bool enabled) { return SetExpanderLevel(board::kExpanderPowerHold, enabled); }

}  // namespace micropixel::platform::reterminal_d1001