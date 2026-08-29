#include "host/time/system_time.hpp"

#include <cstdio>

namespace micropixel::firmware::system_time {

bool IsTrustedUtcTime(std::time_t utc_seconds) {
    return utc_seconds >= kMinimumTrustedUtcSeconds && utc_seconds < kMaximumTrustedUtcSeconds;
}

std::array<char, 6U> FormatBeijingClock(std::time_t utc_seconds) {
    std::array<char, 6U> text{'-', '-', ':', '-', '-', '\0'};
    if (!IsTrustedUtcTime(utc_seconds)) {
        return text;
    }

    const std::time_t beijing_seconds = utc_seconds + static_cast<std::time_t>(kBeijingUtcOffsetSeconds);
    std::tm beijing{};
    if (gmtime_r(&beijing_seconds, &beijing) == nullptr) {
        return text;
    }
    (void)std::snprintf(text.data(), text.size(), "%02d:%02d", beijing.tm_hour, beijing.tm_min);
    return text;
}

}  // namespace micropixel::firmware::system_time
