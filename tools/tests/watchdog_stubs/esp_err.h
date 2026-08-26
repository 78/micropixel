#ifndef MICROPIXEL_TEST_WATCHDOG_STUB_ESP_ERR_H
#define MICROPIXEL_TEST_WATCHDOG_STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_ARG 0x102

const char* esp_err_to_name(esp_err_t error);

#endif
