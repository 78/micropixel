// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "driver/spi_master.h"
enum spi_nand_flash_io_mode_t { SPI_NAND_IO_MODE_SIO, SPI_NAND_IO_MODE_QOUT, SPI_NAND_IO_MODE_QIO };
struct spi_nand_flash_device_t {};
struct spi_nand_flash_config_t {
    spi_device_handle_t device_handle;
    uint8_t gc_factor;
    spi_nand_flash_io_mode_t io_mode;
    unsigned flags;
};
esp_err_t spi_nand_flash_init_device(const spi_nand_flash_config_t*, spi_nand_flash_device_t**);
esp_err_t spi_nand_flash_deinit_device(spi_nand_flash_device_t*);
esp_err_t spi_nand_flash_get_sector_size(spi_nand_flash_device_t*, uint32_t*);
esp_err_t spi_nand_flash_get_capacity(spi_nand_flash_device_t*, uint32_t*);
esp_err_t spi_nand_flash_read_sector(spi_nand_flash_device_t*, uint8_t*, uint32_t);
esp_err_t spi_nand_flash_write_sector(spi_nand_flash_device_t*, const uint8_t*, uint32_t);
esp_err_t spi_nand_flash_trim(spi_nand_flash_device_t*, uint32_t);
esp_err_t spi_nand_flash_sync(spi_nand_flash_device_t*);
