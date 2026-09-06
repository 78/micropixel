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

// Core assignment. The Guest AOT session owns kGuestCore so that a full-screen
// software renderer gets a whole core (and, on ESP32-S31, the PIE unit that only
// exists on core 1). Everything the Host runs itself (LVGL, audio, I2C, USB,
// workers) is pinned to kSystemCore. Tasks that use the FPU must be pinned to a
// core on RISC-V targets; either core satisfies that requirement.
constexpr BaseType_t kGuestCore = 1;
constexpr BaseType_t kSystemCore = 0;

static_assert(kGuestCore != kSystemCore);
static_assert(kGuestCore >= 0 && kGuestCore < static_cast<BaseType_t>(configNUMBER_OF_CORES));
static_assert(kSystemCore >= 0 && kSystemCore < static_cast<BaseType_t>(configNUMBER_OF_CORES));

}  // namespace micropixel::task_policy
