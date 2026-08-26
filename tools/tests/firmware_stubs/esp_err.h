#ifndef MICROPIXEL_TEST_STUB_ESP_ERR_H
#define MICROPIXEL_TEST_STUB_ESP_ERR_H

#ifdef __cplusplus
using esp_err_t = int;
#else
typedef int esp_err_t;
#endif

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102

static inline const char* esp_err_to_name(esp_err_t error) {
    (void)error;
    return "test-error";
}

#endif
