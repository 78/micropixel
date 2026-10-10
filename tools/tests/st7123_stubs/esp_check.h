// SPDX-License-Identifier: Apache-2.0
#pragma once
#define ESP_RETURN_ON_FALSE(a, e, ...) \
    do {                               \
        if (!(a)) return (e);          \
    } while (0)
#define ESP_RETURN_ON_ERROR(a, ...) \
    do {                            \
        esp_err_t s = (a);          \
        if (s != ESP_OK) return s;  \
    } while (0)
#define ESP_GOTO_ON_FALSE(a, e, label, ...) \
    do {                                    \
        if (!(a)) {                         \
            ret = (e);                      \
            goto label;                     \
        }                                   \
    } while (0)
#define ESP_GOTO_ON_ERROR(a, label, ...) \
    do {                                 \
        ret = (a);                       \
        if (ret != ESP_OK) goto label;   \
    } while (0)
