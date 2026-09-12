#ifndef MICROPIXEL_RUNTIME_GUEST_FAILURE_DETAIL_HPP
#define MICROPIXEL_RUNTIME_GUEST_FAILURE_DETAIL_HPP

#include <array>
#include <cstdio>

namespace micropixel::runtime {

// A recoverable decode failure is context, not proof of the trap's cause.
// Bound each field so the WAMR exception survives even a long Guest panic.
[[nodiscard]] inline std::array<char, 256U> FormatGuestTrapDetail(const char* exception, const char* panic,
                                                                  const char* decode_failure) {
    std::array<char, 256U> detail{};
    if (decode_failure[0] != '\0') {
        if (panic[0] != '\0') {
            (void)std::snprintf(detail.data(), detail.size(), "%.72s; last decode error: %.104s; %.48s", panic,
                                decode_failure, exception);
        } else {
            (void)std::snprintf(detail.data(), detail.size(), "last decode error: %.144s; %.80s", decode_failure,
                                exception);
        }
    } else if (panic[0] != '\0') {
        (void)std::snprintf(detail.data(), detail.size(), "%.160s; %.80s", panic, exception);
    } else {
        (void)std::snprintf(detail.data(), detail.size(), "%s", exception);
    }
    return detail;
}

}  // namespace micropixel::runtime

#endif
