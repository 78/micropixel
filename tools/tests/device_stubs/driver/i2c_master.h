// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Minimal ESP-IDF I2C-master surface for Host tests of the board drivers. The
// behaviour lives in fake_i2c_bus.hpp, which a test includes to install devices.
enum i2c_addr_bit_len_t { I2C_ADDR_BIT_LEN_7 = 0 };

struct i2c_device_config_t final {
    i2c_addr_bit_len_t dev_addr_length{};
    uint16_t device_address{};
    uint32_t scl_speed_hz{};
};

using i2c_master_bus_handle_t = void*;
using i2c_master_dev_handle_t = void*;

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* device_out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t* payload, size_t size, int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t* write, size_t write_size,
                                      uint8_t* read, size_t read_size, int timeout_ms);
