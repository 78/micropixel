#ifndef MICROPIXEL_FIRMWARE_SYSTEM_TIME_HPP
#define MICROPIXEL_FIRMWARE_SYSTEM_TIME_HPP

#include <array>
#include <cstdint>
#include <ctime>

namespace micropixel::firmware::system_time {

constexpr std::time_t kMinimumTrustedUtcSeconds = 1704067200;  // 2024-01-01 UTC.
constexpr std::time_t kMaximumTrustedUtcSeconds = 4102444800;  // 2100-01-01 UTC.
constexpr int64_t kBeijingUtcOffsetSeconds = 8LL * 60LL * 60LL;

[[nodiscard]] bool IsTrustedUtcTime(std::time_t utc_seconds);
[[nodiscard]] std::array<char, 6U> FormatBeijingClock(std::time_t utc_seconds);

}  // namespace micropixel::firmware::system_time

#endif
