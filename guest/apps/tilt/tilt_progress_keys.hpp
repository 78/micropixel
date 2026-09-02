#ifndef MICROPIXEL_TILT_PROGRESS_KEYS_HPP
#define MICROPIXEL_TILT_PROGRESS_KEYS_HPP

#include <stdint.h>

namespace tilt {

inline constexpr const char* kLegacyBestTimeKeys[] = {
    "best_01_ms", "best_02_ms", "best_03_ms", "best_04_ms", "best_05_ms", "best_06_ms", "best_07_ms",
    "best_08_ms", "best_09_ms", "best_10_ms", "best_11_ms", "best_12_ms", "best_13_ms",
};
inline constexpr const char* kLegacyBestRatingKeys[] = {
    "rank_01", "rank_02", "rank_03", "rank_04", "rank_05", "rank_06", "rank_07",
    "rank_08", "rank_09", "rank_10", "rank_11", "rank_12", "rank_13",
};
inline constexpr uint32_t kLegacyLevelCount = sizeof(kLegacyBestTimeKeys) / sizeof(kLegacyBestTimeKeys[0]);

static_assert(kLegacyLevelCount == sizeof(kLegacyBestRatingKeys) / sizeof(kLegacyBestRatingKeys[0]));

}  // namespace tilt

#endif
