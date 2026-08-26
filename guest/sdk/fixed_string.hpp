#ifndef MICROPIXEL_SDK_FIXED_STRING_HPP
#define MICROPIXEL_SDK_FIXED_STRING_HPP

#include <stdint.h>

namespace micropixel {

// Allocation-free string builder for short UI and log text in freestanding Guests.
template <uint32_t Capacity>
class FixedString final {
   public:
    static_assert(Capacity >= 2U, "FixedString needs space for content and NUL");

    constexpr void Clear() {
        size_ = 0U;
        bytes_[0] = '\0';
        truncated_ = false;
    }

    // Returns true when the complete value fit. Truncation is sticky until
    // Clear(), so a caller can check truncated() after a sequence of appends.
    bool Append(const char* text) {
        if (text == nullptr) {
            return false;
        }
        while (*text != '\0' && size_ + 1U < Capacity) {
            bytes_[size_++] = *text++;
        }
        bytes_[size_] = '\0';
        if (*text != '\0') {
            truncated_ = true;
            return false;
        }
        return true;
    }

    bool AppendUint(uint64_t value) {
        char reversed[20]{};
        uint32_t count = 0U;
        do {
            reversed[count++] = static_cast<char>('0' + value % 10U);
            value /= 10U;
        } while (value != 0U && count < sizeof(reversed));
        while (count != 0U && size_ + 1U < Capacity) {
            bytes_[size_++] = reversed[--count];
        }
        bytes_[size_] = '\0';
        if (count != 0U) {
            truncated_ = true;
            return false;
        }
        return true;
    }

    bool AppendPadded4(uint32_t value) {
        uint32_t digits = 1U;
        for (uint32_t remaining = value; remaining >= 10U; remaining /= 10U) {
            ++digits;
        }
        bool complete = true;
        while (digits < 4U && size_ + 1U < Capacity) {
            bytes_[size_++] = '0';
            ++digits;
        }
        if (digits < 4U) {
            truncated_ = true;
            complete = false;
        }
        return AppendUint(value) && complete;
    }

    [[nodiscard]] constexpr const char* c_str() const { return bytes_; }
    [[nodiscard]] constexpr uint32_t size() const { return size_; }
    [[nodiscard]] static constexpr uint32_t capacity() { return Capacity - 1U; }
    [[nodiscard]] constexpr bool empty() const { return size_ == 0U; }
    [[nodiscard]] constexpr bool truncated() const { return truncated_; }

   private:
    char bytes_[Capacity]{};
    uint32_t size_{};
    bool truncated_{};
};

}  // namespace micropixel

#endif
