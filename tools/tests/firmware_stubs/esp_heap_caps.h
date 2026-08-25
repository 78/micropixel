#ifndef MICROPIXEL_TEST_STUB_ESP_HEAP_CAPS_H
#define MICROPIXEL_TEST_STUB_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1U << 0U)
#define MALLOC_CAP_INTERNAL (1U << 1U)
#define MALLOC_CAP_SPIRAM (1U << 2U)

static inline void* heap_caps_malloc(size_t size, unsigned capabilities) {
    (void)capabilities;
    return malloc(size);
}

static inline void heap_caps_free(void* memory) { free(memory); }

#endif
