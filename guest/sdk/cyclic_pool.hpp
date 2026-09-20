#ifndef MICROPIXEL_SDK_CYCLIC_POOL_HPP
#define MICROPIXEL_SDK_CYCLIC_POOL_HPP

#include <stdint.h>

namespace micropixel {

// A fixed-capacity overwrite pool for short-lived game objects such as
// particles, trails and score popups. There is no allocation; acquiring
// beyond capacity deterministically reuses the oldest slot, so the caller
// decides how many effects may be alive at once by choosing Capacity.
template <typename T, uint32_t Capacity>
class CyclicPool final {
   public:
    static_assert(Capacity > 0U, "CyclicPool capacity must be positive");

    [[nodiscard]] static constexpr uint32_t capacity() { return Capacity; }

    // Returns the next slot, wrapping around to the oldest one. The slot is
    // handed back as-is; callers reinitialize it.
    [[nodiscard]] T& Acquire() { return items_[cursor_++ % Capacity]; }
    void ResetCursor() { cursor_ = 0U; }
    // Value-initializes every slot and restarts the cursor.
    void Clear() {
        for (T& item : items_) {
            item = T{};
        }
        cursor_ = 0U;
    }

    [[nodiscard]] T& operator[](uint32_t index) { return items_[index]; }
    [[nodiscard]] const T& operator[](uint32_t index) const { return items_[index]; }

    [[nodiscard]] T* begin() { return items_; }
    [[nodiscard]] T* end() { return items_ + Capacity; }
    [[nodiscard]] const T* begin() const { return items_; }
    [[nodiscard]] const T* end() const { return items_ + Capacity; }

   private:
    T items_[Capacity]{};
    uint32_t cursor_{};
};

}  // namespace micropixel

#endif
