// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_lcd_panel_io.h"
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 5
#define ESP_LCD_TOUCH_ST7123_VER_MAJOR 1
#define ESP_LCD_TOUCH_ST7123_VER_MINOR 0
#define ESP_LCD_TOUCH_ST7123_VER_PATCH 2
struct esp_lcd_touch_t;
typedef struct esp_lcd_touch_t* esp_lcd_touch_handle_t;
typedef struct {
    int int_gpio_num;
    int rst_gpio_num;
    struct {
        unsigned interrupt;
        unsigned reset;
    } levels;
    void (*interrupt_callback)(esp_lcd_touch_handle_t);
} esp_lcd_touch_config_t;
typedef struct esp_lcd_touch_t {
    esp_lcd_panel_io_handle_t io;
    esp_err_t (*read_data)(esp_lcd_touch_handle_t);
    bool (*get_xy)(esp_lcd_touch_handle_t, uint16_t*, uint16_t*, uint16_t*, uint8_t*, uint8_t);
    esp_err_t (*get_track_id)(esp_lcd_touch_handle_t, uint8_t*, uint8_t);
    esp_err_t (*del)(esp_lcd_touch_handle_t);
    esp_lcd_touch_config_t config;
    struct {
        struct {
            int owner;
        } lock;
        uint8_t points;
        struct {
            uint8_t track_id;
            uint16_t x, y, strength;
        } coords[5];
    } data;
} esp_lcd_touch_t;
static inline esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t tp,
                                                                  void (*cb)(esp_lcd_touch_handle_t)) {
    (void)tp;
    (void)cb;
    return ESP_OK;
}
