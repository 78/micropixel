#ifndef MICROPIXEL_SDK_TYPES_HPP
#define MICROPIXEL_SDK_TYPES_HPP

#include <stdint.h>

namespace micropixel {

template <typename Reading>
class Sensor;

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
    friend constexpr bool operator<(Duration lhs, Duration rhs) { return lhs.microseconds_ < rhs.microseconds_; }
    friend constexpr bool operator<=(Duration lhs, Duration rhs) { return lhs.microseconds_ <= rhs.microseconds_; }
    friend constexpr bool operator>(Duration lhs, Duration rhs) { return rhs < lhs; }
    friend constexpr bool operator>=(Duration lhs, Duration rhs) { return rhs <= lhs; }

    friend constexpr Duration operator+(Duration lhs, Duration rhs) {
        if (rhs.microseconds_ > UINT64_MAX - lhs.microseconds_) {
            __builtin_trap();
        }
        return Microseconds(lhs.microseconds_ + rhs.microseconds_);
    }

    friend constexpr Duration operator-(Duration lhs, Duration rhs) {
        if (lhs < rhs) {
            __builtin_trap();
        }
        return Microseconds(lhs.microseconds_ - rhs.microseconds_);
    }

    friend constexpr Duration operator*(Duration duration, uint64_t multiplier) {
        if (multiplier != 0U && duration.microseconds_ > UINT64_MAX / multiplier) {
            __builtin_trap();
        }
        return Microseconds(duration.microseconds_ * multiplier);
    }

    friend constexpr Duration operator*(uint64_t multiplier, Duration duration) { return duration * multiplier; }

    friend constexpr Duration operator/(Duration duration, uint64_t divisor) {
        if (divisor == 0U) {
            __builtin_trap();
        }
        return Microseconds(duration.microseconds_ / divisor);
    }

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
    friend constexpr TimePoint operator+(TimePoint point, Duration duration) {
        if (duration.count_microseconds() > UINT64_MAX - point.microseconds_) {
            __builtin_trap();
        }
        return TimePoint{point.microseconds_ + duration.count_microseconds()};
    }
    friend constexpr TimePoint operator+(Duration duration, TimePoint point) { return point + duration; }
    friend constexpr TimePoint operator-(TimePoint point, Duration duration) {
        if (duration.count_microseconds() > point.microseconds_) {
            __builtin_trap();
        }
        return TimePoint{point.microseconds_ - duration.count_microseconds()};
    }
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
    template <typename Reading>
    friend class Sensor;
};

}  // namespace micropixel

#endif
