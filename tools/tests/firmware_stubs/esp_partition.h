#ifndef MICROPIXEL_TEST_STUB_ESP_PARTITION_H
#define MICROPIXEL_TEST_STUB_ESP_PARTITION_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 0xff
#define ESP_PARTITION_MMAP_DATA 0

typedef int esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef uint32_t esp_partition_mmap_handle_t;

typedef struct esp_partition_t {
    uint32_t address;
    uint32_t size;
    const void* flash_chip;
} esp_partition_t;

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char* label);
esp_err_t esp_partition_read(const esp_partition_t* partition, size_t source_offset, void* destination, size_t size);
esp_err_t esp_partition_write(const esp_partition_t* partition, size_t destination_offset, const void* source,
                              size_t size);
esp_err_t esp_partition_erase_range(const esp_partition_t* partition, size_t start_address, size_t size);
esp_err_t esp_partition_mmap(const esp_partition_t* partition, size_t offset, size_t size, int memory,
                             const void** mapped_pointer, esp_partition_mmap_handle_t* handle);
void esp_partition_munmap(esp_partition_mmap_handle_t handle);

#endif
