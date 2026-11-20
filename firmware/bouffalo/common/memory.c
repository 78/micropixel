// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include "esp_heap_caps.h"
#include "mm.h"

/* ESP-IDF permits free() on heap_caps allocations. Return native SDK
 * allocator pointers so unmodified Bundle readers keep that contract. */
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    const uint32_t supported = MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA;
    if ((caps & ~supported) || ((caps & MALLOC_CAP_SPIRAM) && (caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA))))
        return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (!size || (alignment & (alignment - 1U)) || alignment > 256U) return NULL;
    uint32_t flags = caps & MALLOC_CAP_SPIRAM ? MM_FLAG_HEAP_PSRAM_0 : MM_FLAG_HEAP_OCRAM_0;
    uint32_t code = 1U;
    for (size_t a = 4U; a < alignment; a <<= 1U) ++code;
    flags |= code << MM_FLAG_ALIGN_SHIFT;
    return kmalloc(size, flags);
}
void *heap_caps_malloc(size_t size, uint32_t caps) { return heap_caps_aligned_alloc(8U, size, caps); }
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = heap_caps_malloc(count * size, caps);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
void heap_caps_free(void *memory) { kfree(memory); }
void *heap_caps_realloc(void *memory, size_t size, uint32_t caps) {
    if (!size) {
        heap_caps_free(memory);
        return NULL;
    }
    void *replacement = heap_caps_malloc(size, caps);
    if (!replacement) return NULL;
    if (memory) {
        size_t old_size = kmalloc_size(memory);
        memcpy(replacement, memory, old_size < size ? old_size : size);
        heap_caps_free(memory);
    }
    return replacement;
}
