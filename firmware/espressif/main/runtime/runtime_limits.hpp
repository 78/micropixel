#ifndef MICROPIXEL_RUNTIME_RUNTIME_LIMITS_HPP
#define MICROPIXEL_RUNTIME_RUNTIME_LIMITS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::runtime::limits {

inline constexpr uint32_t kEventQueueCapacity = 16U;
inline constexpr uint32_t kMaxTimers = 8U;
inline constexpr uint32_t kMaxSensorHandles = MICROPIXEL_MAX_SENSOR_HANDLES;
inline constexpr uint32_t kMaxGpioHandles = MICROPIXEL_MAX_GPIO_HANDLES;
inline constexpr uint32_t kMaxHapticHandles = MICROPIXEL_MAX_HAPTIC_HANDLES;
inline constexpr uint32_t kMaxResourceRequests = 8U;
inline constexpr uint32_t kMaxBitmaps = 128U;

}  // namespace micropixel::runtime::limits

#endif
