// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdio>
#define ESP_LOGE(tag, format, ...)                        \
    do {                                                  \
        (void)(tag);                                      \
        std::fprintf(stderr, format "\n", ##__VA_ARGS__); \
    } while (0)
#define ESP_LOGD(tag, ...) \
    do {                   \
        (void)(tag);       \
    } while (0)
