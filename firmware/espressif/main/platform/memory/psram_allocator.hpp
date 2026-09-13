// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "esp_heap_caps.h"

namespace micropixel::platform::memory {

// STL allocator that always takes PSRAM. Prefer string_view/span at APIs; use
// these containers only when Host code must own growable text or collections.
template <typename T>
class PsramAllocator {
   public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::true_type;

    constexpr PsramAllocator() noexcept = default;
    template <typename U>
    constexpr PsramAllocator(const PsramAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            std::abort();
        }
        void* memory = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory == nullptr) {
            std::abort();
        }
        return static_cast<T*>(memory);
    }
    void deallocate(T* pointer, std::size_t) noexcept { heap_caps_free(pointer); }
};

template <typename T, typename U>
constexpr bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
    return true;
}

using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

template <typename Key, typename Value, typename Compare = std::less<Key>>
using PsramMap = std::map<Key, Value, Compare, PsramAllocator<std::pair<const Key, Value>>>;

}  // namespace micropixel::platform::memory
