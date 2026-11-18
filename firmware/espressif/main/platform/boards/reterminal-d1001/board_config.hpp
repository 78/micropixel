// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/ledc.h"

namespace micropixel::platform::reterminal_d1001::board {

// Seeed reTerminal D1001: ESP32-P4 with an 800x1280 portrait MIPI-DSI panel.
inline constexpr int32_t kDisplayWidth = 800;
inline constexpr int32_t kDisplayHeight = 1280;
// The vendor timing (60 MHz DPI clock, 880 x 1344 total) scans out at about
// 50.7 Hz; the panel tolerates it. Raise the DPI clock to ~71 MHz only after
// measuring the module, never by guessing.
inline constexpr uint32_t kDisplayRefreshRateHz = 51U;

// System I2C (I2C1): PCA9535 expander, ES8311 codec, PCF8563 RTC and the
// LSM6DS3TR-C IMU.
inline constexpr i2c_port_num_t kSystemI2cPort = I2C_NUM_1;
inline constexpr gpio_num_t kSystemI2cScl = GPIO_NUM_21;
inline constexpr gpio_num_t kSystemI2cSda = GPIO_NUM_20;
inline constexpr uint32_t kSystemI2cFrequencyHz = 400'000;

// Touch I2C (I2C0): the GSL3670 shares this bus with the camera and light
// sensor, so it stays separate from the system bus.
inline constexpr i2c_port_num_t kTouchI2cPort = I2C_NUM_0;
inline constexpr gpio_num_t kTouchI2cScl = GPIO_NUM_38;
inline constexpr gpio_num_t kTouchI2cSda = GPIO_NUM_37;
inline constexpr uint32_t kTouchI2cFrequencyHz = 400'000;
inline constexpr uint8_t kTouchAddress = 0x40U;
inline constexpr uint8_t kTouchMaxPoints = 3U;
inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_16;
// The vendor driver resets the controller through the expander and therefore
// receives the expander bit number, not a GPIO number.
inline constexpr uint8_t kTouchResetExpanderPin = 12U;

// PCA9535 on the system I2C bus. The vendor defaults leave every line an
// output and then latch the rails in this order.
inline constexpr uint8_t kIoExpanderAddress = 0x20U;
inline constexpr uint32_t kExpanderLcdPowerEnable = 1U << 0U;     // P0.0
inline constexpr uint32_t kExpanderCameraEnable = 1U << 1U;       // P0.1
inline constexpr uint32_t kExpanderLcdReset = 1U << 2U;           // P0.2
inline constexpr uint32_t kExpanderCameraPowerDown = 1U << 3U;    // P0.3
inline constexpr uint32_t kExpanderBatteryReadEnable = 1U << 6U;  // P0.6
inline constexpr uint32_t kExpanderBacklightEnable = 1U << 7U;    // P0.7
inline constexpr uint32_t kExpanderPowerHold = 1U << 8U;          // P1.0
inline constexpr uint32_t kExpanderCameraReset = 1U << 9U;        // P1.1
inline constexpr uint32_t kExpanderChargeEnable = 1U << 10U;      // P1.2
inline constexpr uint32_t kExpanderSpeakerAmpEnable = 1U << 11U;  // P1.3
inline constexpr uint32_t kExpanderTouchReset = 1U << 12U;        // P1.4
inline constexpr uint32_t kExpanderOutputMask = 0xFFFFU;

// Panel transport: two MIPI-DSI lanes and the vendor video timing. The DSI
// PHY is powered from LDO channel 3 at 2.5 V.
inline constexpr int kDsiLdoChannel = 3;
inline constexpr int kDsiLdoMillivolts = 2500;
inline constexpr int kDsiLaneCount = 2;
inline constexpr int kDsiLaneBitRateMbps = 1500;
inline constexpr float kDpiClockMHz = 60.0F;
inline constexpr int kPanelHsyncBackPorch = 20;
inline constexpr int kPanelHsyncPulseWidth = 20;
inline constexpr int kPanelHsyncFrontPorch = 40;
inline constexpr int kPanelVBackPorch = 30;
inline constexpr int kPanelVsyncPulseWidth = 4;
inline constexpr int kPanelVsyncFrontPorch = 30;
inline constexpr int kDisplayFramebufferCount = 2;

// Backlight: LEDC PWM on GPIO14. Timer 1 / channel 0 stay clear of the
// application PWM slots (timers/channels 2 and 3).
inline constexpr gpio_num_t kDisplayBacklight = GPIO_NUM_14;
inline constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
inline constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_1;
inline constexpr uint32_t kBacklightMaximumDuty = (1U << 10U) - 1U;

// Audio: ES8311 speaker DAC on I2S0, amplifier enabled from the expander.
inline constexpr gpio_num_t kAudioMclk = GPIO_NUM_33;
inline constexpr gpio_num_t kAudioBitClock = GPIO_NUM_32;
inline constexpr gpio_num_t kAudioWordSelect = GPIO_NUM_31;
inline constexpr gpio_num_t kAudioDataOut = GPIO_NUM_30;
inline constexpr uint8_t kAudioCodecAddress = 0x18U;
inline constexpr uint32_t kAudioSampleRate = 16000U;

// microSD socket on SDMMC slot 0, 4-bit, powered from the P4 on-chip LDO.
inline constexpr int kSdCardLdoChannel = 4;
inline constexpr int kSdCardBusWidth = 4;
inline constexpr gpio_num_t kSdCardClock = GPIO_NUM_43;
inline constexpr gpio_num_t kSdCardCommand = GPIO_NUM_44;
inline constexpr gpio_num_t kSdCardData0 = GPIO_NUM_39;
inline constexpr gpio_num_t kSdCardData1 = GPIO_NUM_40;
inline constexpr gpio_num_t kSdCardData2 = GPIO_NUM_41;
inline constexpr gpio_num_t kSdCardData3 = GPIO_NUM_42;
inline constexpr gpio_num_t kSdCardDetect = GPIO_NUM_45;
inline constexpr gpio_num_t kSdCardPowerEnable = GPIO_NUM_46;

// Power key (green button) and the expander rail latch. The button is
// active-low; a long press is the power-off request.
inline constexpr gpio_num_t kPowerKey = GPIO_NUM_3;
inline constexpr uint32_t kPowerOffLongPressMs = 1000U;

// Battery monitor: ADC on GPIO18 and the charger status input on GPIO15.
inline constexpr gpio_num_t kBatterySense = GPIO_NUM_18;
inline constexpr gpio_num_t kChargerStatus = GPIO_NUM_15;

}  // namespace micropixel::platform::reterminal_d1001::board