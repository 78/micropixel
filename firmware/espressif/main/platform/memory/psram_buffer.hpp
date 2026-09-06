#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include "esp_heap_caps.h"

namespace micropixel::platform::memory {

// A fallible, move-only allocation for simple Host state. Capacity changes are
// explicit; callers stage related allocations before replacing live storage.
template <typename T>
class PsramBuffer final {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);

   public:
    PsramBuffer() = default;
    PsramBuffer(const PsramBuffer&) = delete;
    PsramBuffer& operator=(const PsramBuffer&) = delete;
    PsramBuffer(PsramBuffer&& other) noexcept { Swap(other); }
    PsramBuffer& operator=(PsramBuffer&& other) noexcept {
        if (this != &other) {
            Reset();
            Swap(other);
        }
        return *this;
    }
    ~PsramBuffer() { Reset(); }

    [[nodiscard]] bool Allocate(size_t count) {
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            return false;
        }
        PsramBuffer replacement;
        if (count != 0U) {
            replacement.data_ = static_cast<T*>(heap_caps_aligned_alloc(
                std::max(alignof(T), alignof(void*)), count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (replacement.data_ == nullptr) {
                return false;
            }
            replacement.size_ = count;
            for (size_t index = 0U; index < count; ++index) {
                std::construct_at(replacement.data_ + index);
            }
        }
        Swap(replacement);
        return true;
    }
    void Reset() {
        heap_caps_free(std::exchange(data_, nullptr));
        size_ = 0U;
    }
    void Swap(PsramBuffer& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
    }
    [[nodiscard]] std::span<T> View() const { return {data_, size_}; }
    [[nodiscard]] size_t size() const { return size_; }

   private:
    T* data_{};
    size_t size_{};
};

}  // namespace micropixel::platform::memory
