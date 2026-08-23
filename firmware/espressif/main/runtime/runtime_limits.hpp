#ifndef MICROPIXEL_RUNTIME_RUNTIME_LIMITS_HPP
#define MICROPIXEL_RUNTIME_RUNTIME_LIMITS_HPP

#include <cstdint>

namespace micropixel::runtime::limits {

inline constexpr uint32_t kEventQueueCapacity = 16U;
inline constexpr uint32_t kMaxTimers = 8U;
inline constexpr uint32_t kMaxResourceRequests = 8U;
inline constexpr uint32_t kMaxBitmaps = 128U;

}  // namespace micropixel::runtime::limits

#endif
