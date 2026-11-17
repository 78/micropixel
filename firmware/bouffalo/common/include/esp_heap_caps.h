// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MALLOC_CAP_8BIT 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_SPIRAM 4U
#define MALLOC_CAP_DMA 8U
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps);
void *heap_caps_realloc(void *memory, size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void heap_caps_free(void *memory);
#ifdef __cplusplus
}
#endif
