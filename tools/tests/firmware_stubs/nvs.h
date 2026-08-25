#ifndef MICROPIXEL_TEST_STUB_NVS_H
#define MICROPIXEL_TEST_STUB_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NVS_READONLY 0
#define NVS_READWRITE 1

typedef uint32_t nvs_handle_t;

esp_err_t nvs_open_from_partition(const char* partition_label, const char* namespace_name, int open_mode,
                                  nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);

#endif
