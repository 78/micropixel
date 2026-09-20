// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "esp_err.h"
using spi_host_device_t = int;
using spi_device_handle_t = void*;
constexpr unsigned SPI_TRANS_VARIABLE_CMD = 1, SPI_TRANS_VARIABLE_ADDR = 2, SPI_TRANS_VARIABLE_DUMMY = 4,
                   SPI_TRANS_USE_RXDATA = 8;
constexpr unsigned SPICOMMON_BUSFLAG_QUAD = 1, SPICOMMON_BUSFLAG_MASTER = 2, SPI_DEVICE_HALFDUPLEX = 4;
constexpr int SPI_DMA_CH_AUTO = 0;
struct spi_transaction_t {
    unsigned flags;
    uint16_t cmd;
    unsigned rxlength;
    uint8_t rx_data[4];
};
struct spi_transaction_ext_t {
    spi_transaction_t base;
    unsigned command_bits, address_bits, dummy_bits;
};
struct spi_bus_config_t {
    int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz;
    unsigned flags;
};
struct spi_device_interface_config_t {
    int clock_speed_hz, mode, spics_io_num, queue_size;
    unsigned flags;
};
inline esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t*) { return ESP_OK; }
inline esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int) { return ESP_OK; }
inline esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t* out) {
    static int device;
    *out = &device;
    return ESP_OK;
}
inline esp_err_t spi_bus_remove_device(spi_device_handle_t) { return ESP_OK; }
inline esp_err_t spi_bus_free(spi_host_device_t) { return ESP_OK; }
