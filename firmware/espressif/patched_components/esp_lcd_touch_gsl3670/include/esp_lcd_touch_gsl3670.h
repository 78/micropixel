/*
 * SPDX-FileCopyrightText: 2026 wamr-host contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal GSL3670 touch controller driver for MicroPixel. The vendor's
 * algorithm/firmware bundle is GPL-2.0, so this driver deliberately contains
 * only the I2C register interface and raw report parser. It does not load a
 * firmware image or run Silead's coordinate post-processing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LCD_TOUCH_IO_I2C_GSL3670_ADDRESS (0x40)

#define ESP_LCD_TOUCH_IO_I2C_GSL3670_CONFIG() \
    {                                         \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3670_ADDRESS, \
        .control_phase_bytes = 1,             \
        .dc_bit_offset = 0,                   \
        .lcd_cmd_bits = 8,                    \
        .lcd_param_bits = 8,                  \
        .flags = {                            \
            .disable_control_phase = 1,       \
        },                                    \
    }

// Optional reset path. The D1001 touch reset is routed through the PCA9535
// expander, so the board supplies the pulse instead of a GPIO number. When
// this is not set, the driver falls back to config.rst_gpio_num.
typedef struct {
    esp_err_t (*set_reset)(void *context, bool asserted);
    void *reset_context;
} gsl3670_io_config_t;

esp_err_t esp_lcd_touch_new_i2c_gsl3670(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                        esp_lcd_touch_handle_t *out_touch);

#ifdef __cplusplus
}
#endif