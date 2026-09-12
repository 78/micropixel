// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_8BIT 2U
inline size_t test_cover_allocations{};
inline size_t test_cover_free_bytes = 16U * 1024U * 1024U;
inline bool test_cover_fail_allocation{};
inline size_t heap_caps_get_free_size(unsigned) { return test_cover_free_bytes; }
inline void* heap_caps_aligned_calloc(size_t, size_t count, size_t size, unsigned) {
    if (test_cover_fail_allocation) return nullptr;
    void* memory = std::calloc(count, size);
    if (memory != nullptr) ++test_cover_allocations;
    return memory;
}
inline void heap_caps_free(void* memory) {
    if (memory != nullptr) --test_cover_allocations;
    std::free(memory);
}
