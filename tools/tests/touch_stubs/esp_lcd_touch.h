// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#define ESP_ERR_INVALID_RESPONSE 0x108

using esp_lcd_panel_io_handle_t = void*;
struct esp_lcd_touch_point_data_t {
    uint8_t track_id{};
    uint16_t x{};
    uint16_t y{};
    uint16_t strength{};
};

struct esp_lcd_touch_t {
    esp_lcd_panel_io_handle_t io{};
    struct {
        uint16_t x_max{};
        uint16_t y_max{};
    } config;
    struct {
        uint8_t points{};
        esp_lcd_touch_point_data_t coords[CONFIG_ESP_LCD_TOUCH_MAX_POINTS]{};
        portMUX_TYPE lock{};
    } data;
};
using esp_lcd_touch_handle_t = esp_lcd_touch_t*;

esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int command, void* data, size_t length);
