#ifndef MICROPIXEL_TEST_STUB_NVS_H
#define MICROPIXEL_TEST_STUB_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_NS_NAME_MAX_SIZE 16
#define NVS_KEY_NAME_MAX_SIZE 16
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105

typedef enum {
    NVS_TYPE_ANY,
    NVS_TYPE_U8,
    NVS_TYPE_I8,
    NVS_TYPE_U16,
    NVS_TYPE_I16,
    NVS_TYPE_U32,
    NVS_TYPE_I32,
    NVS_TYPE_U64,
    NVS_TYPE_I64,
    NVS_TYPE_BLOB,
    NVS_TYPE_STR
} nvs_type_t;
struct TestNvsIterator;
typedef struct TestNvsIterator* nvs_iterator_t;
typedef struct {
    char key[16];
    nvs_type_t type;
} nvs_entry_info_t;

typedef uint32_t nvs_handle_t;

typedef struct {
    size_t used_entries;
    size_t free_entries;
    size_t available_entries;
    size_t total_entries;
    size_t namespace_count;
} nvs_stats_t;
esp_err_t nvs_get_stats(const char* partition_label, nvs_stats_t* stats);

esp_err_t nvs_open_from_partition(const char* partition_label, const char* namespace_name, int open_mode,
                                  nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_i32(nvs_handle_t handle, const char* key, int32_t* out_value);
esp_err_t nvs_set_i32(nvs_handle_t handle, const char* key, int32_t value);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_erase_all(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_find_key(nvs_handle_t, const char*, nvs_type_t*);
esp_err_t nvs_entry_find_in_handle(nvs_handle_t, nvs_type_t, nvs_iterator_t*);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*);
esp_err_t nvs_entry_next(nvs_iterator_t*);
void nvs_release_iterator(nvs_iterator_t);
#endif
