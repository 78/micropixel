// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NVS_KEY_NAME_MAX_SIZE 16
#define NVS_NS_NAME_MAX_SIZE 16
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
typedef uintptr_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef enum {
    NVS_TYPE_U8 = 1,
    NVS_TYPE_I8 = 0x11,
    NVS_TYPE_U16 = 2,
    NVS_TYPE_I16 = 0x12,
    NVS_TYPE_U32 = 4,
    NVS_TYPE_I32 = 0x14,
    NVS_TYPE_U64 = 8,
    NVS_TYPE_I64 = 0x18,
    NVS_TYPE_STR = 0x21,
    NVS_TYPE_BLOB = 0x42,
    NVS_TYPE_ANY = 0xff
} nvs_type_t;
typedef struct {
    char namespace_name[16];
    char key[16];
    nvs_type_t type;
} nvs_entry_info_t;
typedef struct mpx_nvs_iterator *nvs_iterator_t;
esp_err_t nvs_open_from_partition(const char *, const char *, nvs_open_mode_t, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_find_key(nvs_handle_t, const char *, nvs_type_t *);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_entry_find_in_handle(nvs_handle_t, nvs_type_t, nvs_iterator_t *);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t *);
esp_err_t nvs_entry_next(nvs_iterator_t *);
void nvs_release_iterator(nvs_iterator_t);
#ifdef __cplusplus
}
#endif
