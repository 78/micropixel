#pragma once

#include "freertos/FreeRTOS.h"

namespace micropixel::task_policy {

// Keep MicroPixel below ESP-IDF's high-priority IPC, timer, event and network
// tasks while preserving the system-over-Guest scheduling boundary.
constexpr UBaseType_t kAudioPriority = 8U;
constexpr UBaseType_t kTouchPriority = 8U;
constexpr UBaseType_t kHostPriority = 7U;
constexpr UBaseType_t kDisplayPriority = 6U;
constexpr UBaseType_t kCapturePriority = 6U;
constexpr UBaseType_t kGuestPriority = 5U;
constexpr UBaseType_t kAssetWorkerPriority = 3U;

static_assert(kAudioPriority > kHostPriority);
static_assert(kHostPriority > kCapturePriority);
static_assert(kCapturePriority > kGuestPriority);
static_assert(kTouchPriority > kDisplayPriority);
static_assert(kDisplayPriority > kGuestPriority);
static_assert(kGuestPriority > kAssetWorkerPriority);
static_assert(kAudioPriority < configMAX_PRIORITIES);

}  // namespace micropixel::task_policy
