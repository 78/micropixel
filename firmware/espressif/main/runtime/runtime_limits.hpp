#ifndef MICROPIXEL_RUNTIME_RUNTIME_LIMITS_HPP
#define MICROPIXEL_RUNTIME_RUNTIME_LIMITS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "sdkconfig.h"

namespace micropixel::runtime::limits {

inline constexpr uint32_t kEventQueueCapacity = 16U;
inline constexpr uint32_t kMaxTimers = 8U;
// Concurrent handle slots per session. These are Host policy, not wire
// contract: the Guest learns exhaustion through RESOURCE_EXHAUSTED on OPEN.
inline constexpr uint32_t kMaxSensorHandles = 8U;
inline constexpr uint32_t kMaxGpioHandles = 16U;
inline constexpr uint32_t kMaxHapticHandles = 2U;
inline constexpr uint32_t kMaxResourceRequests = 8U;
#if defined(CONFIG_MICROPIXEL_MAX_BITMAPS)
inline constexpr uint32_t kMaxBitmaps = CONFIG_MICROPIXEL_MAX_BITMAPS;
#else
inline constexpr uint32_t kMaxBitmaps = 128U;
#endif

}  // namespace micropixel::runtime::limits

#endif
