#ifndef MICROPIXEL_TEST_STUB_SPI_FLASH_MMAP_H
#define MICROPIXEL_TEST_STUB_SPI_FLASH_MMAP_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SPI_FLASH_MMU_PAGE_SIZE (64U * 1024U)
#define SPI_FLASH_MMAP_FLAG_DATA 0U

typedef uint32_t spi_flash_mmap_handle_t;

esp_err_t spi_flash_mmap_pages(const int* pages, size_t page_count, uint32_t memory, const void** mapped_pointer,
                               spi_flash_mmap_handle_t* handle);
void spi_flash_munmap(spi_flash_mmap_handle_t handle);

#endif
