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

    bool AppendInt(int64_t value) {
        if (value >= 0) {
            return AppendUint(static_cast<uint64_t>(value));
        }
        bool complete = Append("-");
        const uint64_t magnitude = static_cast<uint64_t>(-(value + 1)) + 1U;
        return AppendUint(magnitude) && complete;
    }

    // Appends `value` rounded to `decimals` digits after the point (0..6),
    // e.g. AppendFixed(-1.2345F, 2) -> "-1.23".
    bool AppendFixed(float value, uint32_t decimals = 2U) {
        if (decimals == 0U) {
            return AppendInt(static_cast<int64_t>(value));
        }
        if (decimals > 6U) {
            decimals = 6U;
        }
        uint32_t scale = 1U;
        for (uint32_t index = 0U; index < decimals; ++index) {
            scale *= 10U;
        }
        const bool negative = value < 0.0F;
        const float magnitude = negative ? -value : value;
        const auto scaled = static_cast<uint64_t>(magnitude * static_cast<float>(scale) + 0.5F);
        bool complete = true;
        if (negative && scaled != 0U) {
            complete = Append("-");
        }
        complete = AppendUint(scaled / scale) && complete;
        complete = Append(".") && complete;
        uint64_t fraction = scaled % scale;
        for (uint32_t divisor = scale / 10U; divisor > 1U; divisor /= 10U) {
            if (fraction < divisor) {
                complete = Append("0") && complete;
            } else {
                break;
            }
        }
        return AppendUint(fraction) && complete;
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
