#ifndef MICROPIXEL_TEST_STUB_ESP_MEMORY_UTILS_H
#define MICROPIXEL_TEST_STUB_ESP_MEMORY_UTILS_H

#include <stdbool.h>

static inline bool esp_ptr_external_ram(const void* pointer) {
    (void)pointer;
    return false;
}

#endif
