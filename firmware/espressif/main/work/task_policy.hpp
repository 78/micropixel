#pragma once

#include "freertos/FreeRTOS.h"

namespace micropixel::task_policy {

// Keep MicroPixel below ESP-IDF's high-priority IPC, timer, event and network
// tasks while preserving the system-over-Guest scheduling boundary.
constexpr UBaseType_t kAudioPriority = 8U;
constexpr UBaseType_t kAudioDecodePriority = 7U;
constexpr UBaseType_t kI2cPriority = 8U;
constexpr UBaseType_t kHostPriority = 7U;
constexpr UBaseType_t kDisplayPriority = 6U;
constexpr UBaseType_t kUsbLocalControlPriority = 6U;
constexpr UBaseType_t kRemoteControlPriority = 4U;
constexpr UBaseType_t kGuestPriority = 3U;
constexpr UBaseType_t kAssetWorkerPriority = 2U;

static_assert(kAudioPriority > kHostPriority);
static_assert(kAudioPriority > kAudioDecodePriority);
static_assert(kAudioDecodePriority > kGuestPriority);
static_assert(kHostPriority > kUsbLocalControlPriority);
static_assert(kUsbLocalControlPriority > kGuestPriority);
static_assert(kI2cPriority > kDisplayPriority);
static_assert(kDisplayPriority > kGuestPriority);
static_assert(kRemoteControlPriority > kGuestPriority);
static_assert(kRemoteControlPriority > kAssetWorkerPriority);
static_assert(kGuestPriority > kAssetWorkerPriority);
static_assert(kAudioPriority < configMAX_PRIORITIES);

}  // namespace micropixel::task_policy
