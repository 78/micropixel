// SPDX-License-Identifier: Apache-2.0
#include "platform/boards/reterminal-d1001/touch_hardware.hpp"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gsl3670.h"
#include "esp_log.h"
#include "platform/boards/reterminal-d1001/board_config.hpp"
#include "platform/boards/reterminal-d1001/board_hardware.hpp"
#include "platform/boards/reterminal-d1001/d1001_state.hpp"

namespace micropixel::platform::reterminal_d1001 {
namespace {

constexpr char kTag[] = "d1001_touch";

esp_err_t SetTouchReset(void* context, bool asserted) {
    auto* hardware = static_cast<BoardHardware*>(context);
    return hardware != nullptr ? hardware->SetTouchReset(asserted) : ESP_ERR_INVALID_ARG;
}

}  // namespace

esp_err_t InitializeTouchHardware(D1001State& state, BoardHardware& hardware) {
    if (state.touch_i2c_bus != nullptr || state.touch != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = board::kTouchI2cPort;
    bus_config.sda_io_num = board::kTouchI2cSda;
    bus_config.scl_io_num = board::kTouchI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7U;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &state.touch_i2c_bus), kTag, "create touch I2C bus failed");

    // Build the panel IO config field by field: the C macro leaves several fields
    // implicit, which triggers -Werror=missing-field-initializers in C++.
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3670_ADDRESS;
    io_config.scl_speed_hz = board::kTouchI2cFrequencyHz;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.flags.disable_control_phase = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(state.touch_i2c_bus, &io_config, &state.touch_io), kTag,
                        "create GSL3670 panel IO failed");

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = static_cast<uint16_t>(board::kDisplayWidth);
    touch_config.y_max = static_cast<uint16_t>(board::kDisplayHeight);
    // The reset line lives on the expander, so the driver pulse goes through
    // the callback instead of a GPIO number.
    gsl3670_io_config_t reset_io{};
    reset_io.set_reset = &SetTouchReset;
    reset_io.reset_context = &hardware;
    touch_config.driver_data = &reset_io;
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    // MVP uses EspLcdTouchInput's 10 ms polling path. GPIO16 stays available
    // for the interrupt-driven variant once the controller report cadence is
    // validated on hardware.
    touch_config.int_gpio_num = GPIO_NUM_NC;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    touch_config.flags.swap_xy = 0;
    touch_config.flags.mirror_x = 1;
    touch_config.flags.mirror_y = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gsl3670(state.touch_io, &touch_config, &state.touch), kTag,
                        "create GSL3670 touch controller failed");
    return ESP_OK;
}

}  // namespace micropixel::platform::reterminal_d1001