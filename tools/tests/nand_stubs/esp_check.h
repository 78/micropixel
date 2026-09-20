// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_RETURN_ON_ERROR(expression, tag, message) \
    do {                                              \
        const auto error = (expression);              \
        if (error != ESP_OK) return error;            \
    } while (0)
