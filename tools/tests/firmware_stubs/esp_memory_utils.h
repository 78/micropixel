#ifndef MICROPIXEL_TEST_STUB_ESP_MEMORY_UTILS_H
#define MICROPIXEL_TEST_STUB_ESP_MEMORY_UTILS_H

#include <stdbool.h>

#ifdef MICROPIXEL_TEST_INTERNAL_RAM
bool micropixel_test_ptr_internal(const void* pointer);
#endif

static inline bool esp_ptr_internal(const void* pointer) {
#ifdef MICROPIXEL_TEST_INTERNAL_RAM
    return micropixel_test_ptr_internal(pointer);
#else
    return pointer != 0;
#endif
}

static inline bool esp_ptr_external_ram(const void* pointer) {
    (void)pointer;
    return false;
}

#endif
