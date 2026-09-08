#ifndef MICROPIXEL_TEST_STUB_ESP_HEAP_CAPS_H
#define MICROPIXEL_TEST_STUB_ESP_HEAP_CAPS_H

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1U << 0U)
#define MALLOC_CAP_INTERNAL (1U << 1U)
#define MALLOC_CAP_SPIRAM (1U << 2U)

#if defined(MICROPIXEL_TEST_TRACK_PSRAM) || defined(MICROPIXEL_TEST_TRACK_HEAP)
void* micropixel_test_psram_allocate(size_t size);
void micropixel_test_psram_free(void* memory);
#endif

static inline void* heap_caps_malloc(size_t size, unsigned capabilities) {
    (void)capabilities;
#ifdef MICROPIXEL_TEST_TRACK_HEAP
    return micropixel_test_psram_allocate(size);
#else
    return malloc(size);
#endif
}

static inline void* heap_caps_calloc(size_t count, size_t size, unsigned capabilities) {
    (void)capabilities;
    return calloc(count, size);
}

static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned capabilities) {
    (void)alignment;
    (void)capabilities;
#if defined(MICROPIXEL_TEST_TRACK_PSRAM) || defined(MICROPIXEL_TEST_TRACK_HEAP)
    return micropixel_test_psram_allocate(size);
#else
    return malloc(size);
#endif
}

static inline void heap_caps_free(void* memory) {
#if defined(MICROPIXEL_TEST_TRACK_PSRAM) || defined(MICROPIXEL_TEST_TRACK_HEAP)
    micropixel_test_psram_free(memory);
#else
    free(memory);
#endif
}

#endif
