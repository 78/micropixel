#ifndef MICROPIXEL_SNAKE_GAMEKIT_CYCLIC_POOL_HPP
#define MICROPIXEL_SNAKE_GAMEKIT_CYCLIC_POOL_HPP

#include <stdint.h>

namespace snake::gamekit {

// A fixed-capacity overwrite pool for short-lived game objects. There is no
// allocation and acquiring beyond capacity deterministically reuses the oldest
// slot. This remains a Snake-internal candidate until another game needs it.
template <typename T, uint32_t Capacity>
class CyclicPool final {
   public:
    static_assert(Capacity > 0U, "CyclicPool capacity must be positive");

    [[nodiscard]] T& Acquire() { return items_[cursor_++ % Capacity]; }
    void ResetCursor() { cursor_ = 0U; }

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

}  // namespace snake::gamekit

#endif
