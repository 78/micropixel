#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
using i2c_master_dev_handle_t = void*;
esp_err_t i2c_master_transmit_receive(void*, const uint8_t*, size_t, uint8_t*, size_t, int);
esp_err_t i2c_master_transmit(void*, const uint8_t*, size_t, int);
