/*
 * SPDX-FileCopyrightText: 2023 wngfra
 * SPDX-License-Identifier: Apache-2.0
 *
 * Vendored from wngfra/esp_io_expander_pca9535 1.0.1 for the Seeed
 * reTerminal D1001 rail expander.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a PCA9535(C) IO expander object
 *
 * @param[in]  i2c_bus    I2C bus handle. Obtained from `i2c_new_master_bus()`
 * @param[in]  dev_addr   I2C device address of chip. Can be `ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_XX` or `ESP_IO_EXPANDER_I2C_PCA9535C_ADDRESS_XXX`.
 * @param[out] handle_ret Handle to created IO expander object
 *
 * @return
 *      - ESP_OK: Success, otherwise returns ESP_ERR_xxx
 */
esp_err_t esp_io_expander_new_i2c_pca9535(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr, esp_io_expander_handle_t *handle_ret);

/**
 * @brief I2C address of the PCA9535
 *
 * The 8-bit address format for the PCA9535 is as follows:
 *
 *                (Slave Address)
 *     ┌─────────────────┷─────────────────┐
 *  ┌─────┐─────┐─────┐─────┐─────┐─────┐─────┐─────┐
 *  |  0  |  1  |  0  |  0  | A2  | A1  | A0  | R/W |
 *  └─────┘─────┘─────┘─────┘─────┘─────┘─────┘─────┘
 *     └────────┯────────┘     └─────┯──────┘
 *           (Fixed)        (Hareware Selectable)
 *
 * And the 7-bit slave address is the most important data for users.
 * For example, if a PCA9535 chip's A0,A1,A2 are connected to GND, it's 7-bit slave address is 0b0100000.
 * Then users can use `ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_000` to init it.
 */
enum esp_io_expander_pca_9535_address {
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_000 = 0b0100000,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001 = 0b0100001,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_010 = 0b0100010,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_011 = 0b0100011,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_100 = 0b0100000,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_101 = 0b0100101,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_110 = 0b0100110,
    ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_111 = 0b0100111,
};

#ifdef __cplusplus
}
#endif