#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "sdkconfig.h"

namespace micropixel::platform::ksdiy_p4c5::board {

// The ST7102 panel scans out 480x800 portrait. Landscape is a build-time
// choice: LVGL renders 800x480 and the LVGL adapter rotates each flush with
// the PPA into the portrait DPI framebuffers.
#if CONFIG_MICROPIXEL_KSDIY_P4C5_LANDSCAPE
inline constexpr bool kLandscape = true;
#else
inline constexpr bool kLandscape = false;
#endif

inline constexpr int32_t kPanelWidth = 480;
inline constexpr int32_t kPanelHeight = 800;
inline constexpr int32_t kDisplayWidth = kLandscape ? kPanelHeight : kPanelWidth;
inline constexpr int32_t kDisplayHeight = kLandscape ? kPanelWidth : kPanelHeight;
inline constexpr uint32_t kDisplayRefreshRateHz = 60U;

// Panel, backlight, DSI and touch pins are owned by kevincoooool/ksdiy_p4c5_bsp.
inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_23;

// Touch, LSM6DS3TR-C, AXP2101 and ES8311 share the BSP's I2C0 bus.
inline constexpr i2c_port_t kI2cPort = I2C_NUM_0;
inline constexpr uint8_t kPmicI2cAddress = 0x34U;

inline constexpr int kAudioI2sPort = 0;
inline constexpr gpio_num_t kAudioMasterClock = GPIO_NUM_13;
inline constexpr gpio_num_t kAudioBitClock = GPIO_NUM_12;
inline constexpr gpio_num_t kAudioWordSelect = GPIO_NUM_10;
inline constexpr gpio_num_t kAudioDataOut = GPIO_NUM_9;
inline constexpr gpio_num_t kAudioAmplifierEnable = GPIO_NUM_3;
inline constexpr uint8_t kAudioCodecI2cAddress = 0x18U;
inline constexpr uint32_t kAudioSampleRate = 32000U;

}  // namespace micropixel::platform::ksdiy_p4c5::board
