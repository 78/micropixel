#ifndef MICROPIXEL_TEST_STUB_ESP_ERR_H
#define MICROPIXEL_TEST_STUB_ESP_ERR_H

#ifdef __cplusplus
using esp_err_t = int;
#else
typedef int esp_err_t;
#endif

#define ESP_OK 0

static inline const char* esp_err_to_name(esp_err_t error) {
    (void)error;
    return "test-error";
}

#endif
