#ifndef MICROPIXEL_SDK_TYPES_HPP
#define MICROPIXEL_SDK_TYPES_HPP

#include <stdint.h>

namespace micropixel {

class Duration final {
   public:
    constexpr Duration() = default;

    [[nodiscard]] static constexpr Duration Microseconds(uint64_t value) { return Duration{RawMicroseconds{}, value}; }
    [[nodiscard]] static constexpr Duration Milliseconds(uint64_t value) {
        if (value > UINT64_MAX / 1000U) {
            __builtin_trap();
        }
        return Duration{RawMicroseconds{}, value * 1000U};
    }
    [[nodiscard]] static constexpr Duration Seconds(uint64_t value) {
        if (value > UINT64_MAX / 1000000U) {
            __builtin_trap();
        }
        return Duration{RawMicroseconds{}, value * 1000000U};
    }
    [[nodiscard]] constexpr uint64_t count_microseconds() const { return microseconds_; }

    friend constexpr bool operator==(Duration, Duration) = default;

   private:
    struct RawMicroseconds {};
    explicit constexpr Duration(RawMicroseconds, uint64_t value) : microseconds_(value) {}

    uint64_t microseconds_{};
};

namespace literals {

[[nodiscard]] constexpr Duration operator""_us(unsigned long long value) {
    return Duration::Microseconds(static_cast<uint64_t>(value));
}

[[nodiscard]] constexpr Duration operator""_ms(unsigned long long value) {
    if (value > UINT64_MAX / 1000U) {
        __builtin_trap();
    }
    return Duration::Milliseconds(static_cast<uint64_t>(value));
}

[[nodiscard]] constexpr Duration operator""_s(unsigned long long value) {
    if (value > UINT64_MAX / 1000000U) {
        __builtin_trap();
    }
    return Duration::Seconds(static_cast<uint64_t>(value));
}

}  // namespace literals

// A value on the application Clock timeline. Only the Runtime can create a
// non-zero TimePoint; applications obtain one from Clock::Now() or an Event.
class TimePoint final {
   public:
    constexpr TimePoint() = default;

    [[nodiscard]] constexpr uint64_t microseconds() const { return microseconds_; }

    friend constexpr bool operator==(TimePoint, TimePoint) = default;
    friend constexpr bool operator<(TimePoint lhs, TimePoint rhs) { return lhs.microseconds_ < rhs.microseconds_; }
    friend constexpr bool operator<=(TimePoint lhs, TimePoint rhs) { return lhs.microseconds_ <= rhs.microseconds_; }
    friend constexpr bool operator>(TimePoint lhs, TimePoint rhs) { return rhs < lhs; }
    friend constexpr bool operator>=(TimePoint lhs, TimePoint rhs) { return rhs <= lhs; }
    friend constexpr Duration operator-(TimePoint lhs, TimePoint rhs) {
        if (lhs < rhs) {
            __builtin_trap();
        }
        return Duration::Microseconds(lhs.microseconds_ - rhs.microseconds_);
    }

   private:
    explicit constexpr TimePoint(uint64_t microseconds) : microseconds_(microseconds) {}

    uint64_t microseconds_{};

    friend class Application;
    friend class Clock;
};

}  // namespace micropixel

#endif
