/*
 * SPDX-FileCopyrightText: 2026 wamr-host contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal GSL3670 driver: reset, the vendor register bring-up sequence and
 * raw report parsing. See the header for why the Silead algorithm/firmware is
 * not part of this component.
 */
#include "esp_lcd_touch_gsl3670.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "gsl3670"

#define GSL3670_REPORT_REG 0x80
#define GSL3670_MAX_POINTS 3
// The vendor reads 44 bytes from the report register: one count byte plus ten
// four-byte point slots. Only the first three are used by this panel.
#define GSL3670_REPORT_BYTES 44

typedef struct {
    esp_lcd_touch_t base;
    gsl3670_io_config_t io;
    uint16_t x[GSL3670_MAX_POINTS];
    uint16_t y[GSL3670_MAX_POINTS];
    uint8_t count;
} gsl3670_touch_t;

static esp_err_t gsl3670_write_u8(esp_lcd_touch_handle_t tp, uint8_t reg, uint8_t value) {
    return esp_lcd_panel_io_tx_param(tp->io, reg, &value, 1);
}

static esp_err_t gsl3670_set_reset(esp_lcd_touch_handle_t tp, bool asserted) {
    gsl3670_touch_t* touch = (gsl3670_touch_t*)tp;
    if (touch->io.set_reset != NULL) {
        return touch->io.set_reset(touch->io.reset_context, asserted);
    }
    const int rst = tp->config.rst_gpio_num;
    if (rst < 0) {
        return ESP_OK;
    }
    return gpio_set_level((gpio_num_t)rst, asserted ? 0 : 1);
}

static esp_err_t gsl3670_reset(esp_lcd_touch_handle_t tp) {
    ESP_RETURN_ON_ERROR(gsl3670_set_reset(tp, true), TAG, "assert reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gsl3670_set_reset(tp, false), TAG, "release reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t gsl3670_read_data(esp_lcd_touch_handle_t tp) {
    if (tp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    gsl3670_touch_t* touch = (gsl3670_touch_t*)tp;
    uint8_t report[GSL3670_REPORT_BYTES] = {0};
    esp_err_t err = esp_lcd_panel_io_rx_param(tp->io, GSL3670_REPORT_REG, report, sizeof(report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read report failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t count = report[0];
    if (count > GSL3670_MAX_POINTS) {
        count = GSL3670_MAX_POINTS;
    }
    for (uint8_t i = 0; i < count; ++i) {
        const uint8_t* point = &report[1 + i * 4];
        // x = 12-bit value split across bytes 2/3; y = 16-bit value in 0/1.
        touch->x[i] = (uint16_t)(((point[3] & 0x0FU) << 8) | point[2]);
        touch->y[i] = (uint16_t)((point[1] << 8) | point[0]);
    }
    touch->count = count;
    return ESP_OK;
}

static bool gsl3670_get_xy(esp_lcd_touch_handle_t tp, uint16_t* x, uint16_t* y, uint16_t* strength, uint8_t* point_num,
                           uint8_t max_point_num) {
    if (tp == NULL || x == NULL || y == NULL || point_num == NULL || max_point_num == 0) {
        return false;
    }
    gsl3670_touch_t* touch = (gsl3670_touch_t*)tp;
    portENTER_CRITICAL(&tp->data.lock);
    uint8_t count = touch->count;
    if (count > max_point_num) {
        count = max_point_num;
    }
    *point_num = count;
    for (uint8_t i = 0; i < count; ++i) {
        x[i] = touch->x[i];
        y[i] = touch->y[i];
        if (strength != NULL) {
            strength[i] = 0;
        }
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return count > 0;
}

static esp_err_t gsl3670_enter_sleep(esp_lcd_touch_handle_t tp) {
    (void)tp;
    return ESP_OK;
}

static esp_err_t gsl3670_exit_sleep(esp_lcd_touch_handle_t tp) {
    (void)tp;
    return ESP_OK;
}

static esp_err_t gsl3670_del(esp_lcd_touch_handle_t tp) {
    if (tp == NULL) {
        return ESP_OK;
    }
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback != NULL) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t gsl3670_write_bc_zero(esp_lcd_touch_handle_t tp) {
    const uint8_t zeros[4] = {0};
    return esp_lcd_panel_io_tx_param(tp->io, 0xBC, zeros, sizeof(zeros));
}

// Vendor register bring-up, minus the GPL firmware blob and algorithm. The
// controller runs its factory firmware; raw coordinates are reported at 0x80.
// Validated on hardware: the factory image alone does not start reporting, so
// touch remains unavailable until a redistribution-compatible vendor firmware
// is obtained (see README.md).
static esp_err_t gsl3670_startup(esp_lcd_touch_handle_t tp) {
    ESP_RETURN_ON_ERROR(gsl3670_reset(tp), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(gsl3670_write_u8(tp, 0x88, 0x01), TAG, "write 0x88 failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl3670_write_u8(tp, 0xE4, 0x04), TAG, "write 0xE4 failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl3670_write_bc_zero(tp), TAG, "write 0xBC failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(gsl3670_write_u8(tp, 0xE0, 0x00), TAG, "write 0xE0 failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t esp_lcd_touch_new_i2c_gsl3670(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t* config,
                                        esp_lcd_touch_handle_t* out_touch) {
    if (io == NULL || config == NULL || out_touch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_touch = NULL;

    gsl3670_touch_t* touch = (gsl3670_touch_t*)calloc(1, sizeof(gsl3670_touch_t));
    if (touch == NULL) {
        return ESP_ERR_NO_MEM;
    }
    touch->base.io = io;
    touch->base.read_data = gsl3670_read_data;
    touch->base.get_xy = gsl3670_get_xy;
    touch->base.enter_sleep = gsl3670_enter_sleep;
    touch->base.exit_sleep = gsl3670_exit_sleep;
    touch->base.del = gsl3670_del;
    touch->base.data.lock.owner = portMUX_FREE_VAL;
    memcpy(&touch->base.config, config, sizeof(esp_lcd_touch_config_t));
    if (config->driver_data != NULL) {
        memcpy(&touch->io, config->driver_data, sizeof(touch->io));
    }

    if (touch->base.config.int_gpio_num != GPIO_NUM_NC) {
        gpio_config_t int_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = touch->base.config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
            .pin_bit_mask = 1ULL << touch->base.config.int_gpio_num,
        };
        esp_err_t err = gpio_config(&int_config);
        if (err != ESP_OK) {
            free(touch);
            return err;
        }
    }

    esp_err_t err = gsl3670_startup(&touch->base);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "startup failed: %s", esp_err_to_name(err));
        gsl3670_del(&touch->base);
        return err;
    }
    *out_touch = &touch->base;
    ESP_LOGI(TAG, "ready at I2C address 0x%02x (raw report mode)", ESP_LCD_TOUCH_IO_I2C_GSL3670_ADDRESS);
    return ESP_OK;
}