// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "driver/gpio.h"

// SenseCAP Watcher (Seeed Studio) wiring facts.
//
// Pin numbers are taken from the vendor board support package, which is the
// authority for this hardware:
//   Seeed-Studio/SenseCAP-Watcher-Firmware, components/sensecap-watcher
// The panel is an SPD2010 driven over QSPI and reports 412x412; its touch
// controller shares the panel part number and sits on a dedicated I2C bus.
//
// This header records wiring only. Directions, active levels, power sequencing
// order and register configuration belong to the board implementation, and are
// established as each peripheral is brought up and verified on hardware.

namespace micropixel::platform::sensecap_watcher::board {

// --- Display: SPD2010, 412x412, QSPI on SPI3_HOST ---
inline constexpr int32_t kDisplayWidth = 412;
inline constexpr int32_t kDisplayHeight = 412;
inline constexpr gpio_num_t kLcdPclk = GPIO_NUM_7;
inline constexpr gpio_num_t kLcdData0 = GPIO_NUM_9;
inline constexpr gpio_num_t kLcdData1 = GPIO_NUM_1;
inline constexpr gpio_num_t kLcdData2 = GPIO_NUM_14;
inline constexpr gpio_num_t kLcdData3 = GPIO_NUM_13;
inline constexpr gpio_num_t kLcdCs = GPIO_NUM_45;
inline constexpr gpio_num_t kLcdBacklight = GPIO_NUM_8;
inline constexpr uint32_t kLcdPixelClockHz = 40 * 1000 * 1000;
// QSPI command framing, as the vendor driver configures it: 32-bit commands
// with 8-bit parameters in SPI mode 3, over four data lines.
inline constexpr int kLcdCommandBits = 32;
inline constexpr int kLcdParameterBits = 8;
inline constexpr int kLcdSpiMode = 3;
// The backlight is a PWM duty on a plain GPIO and is lit by a high level
// (DRV_LCD_BL_ON_LEVEL is 1 and the vendor does not invert the output).
inline constexpr bool kLcdBacklightActiveHigh = true;
// The vendor declares both a DC line and QSPI data 1 on GPIO1. QSPI panels do
// not use DC, and the SPD2010 carries its own GRAM, so no command/data line is
// reserved here.
inline constexpr bool kLcdUsesCommandDataPin = false;
inline constexpr bool kLcdHasResetPin = false;  // The vendor leaves reset unconnected.

// --- Touch: SPD2010 companion, dedicated I2C bus ---
inline constexpr int kTouchI2cPort = 1;
inline constexpr gpio_num_t kTouchSda = GPIO_NUM_39;
inline constexpr gpio_num_t kTouchScl = GPIO_NUM_38;
inline constexpr uint32_t kTouchI2cClockHz = 400000U;

// --- Shared control I2C: audio codec and IO expander ---
inline constexpr int kControlI2cPort = 0;
inline constexpr gpio_num_t kControlSda = GPIO_NUM_47;
inline constexpr gpio_num_t kControlScl = GPIO_NUM_48;
// The board support package does not publish this bus speed. 100 kHz is used
// until the hardware confirms a higher rate; the touch bus is documented
// separately at 400 kHz.
inline constexpr uint32_t kControlI2cClockHz = 100000U;

// --- Audio: ES8311 (output) and ES7243E (input) on one I2S bus ---
// 16 kHz, the rate the shared Opus clip path decodes at, so playback reaches the
// codec without any resampling. The rate has to be 16 kHz or an integer multiple
// of it: the vendor firmware is configured for 24 kHz on this codec, and at
// 24000/16000 = 1.5 the clip path clamps its integer upsampler to 1 and plays
// 16 kHz audio as if it were 24 kHz, which sounds fast and sharp. The ES8311
// takes 16 kHz with the shared sink's MCLK = 256 x fs = 4.096 MHz, exactly as
// szpi-esp32s3 drives the same codec. The ES7243E microphone shares the bus and
// is not driven yet, so it does not constrain the rate.
inline constexpr uint32_t kAudioSampleRate = 16000U;
inline constexpr gpio_num_t kAudioMclk = GPIO_NUM_10;
inline constexpr gpio_num_t kAudioWs = GPIO_NUM_12;
inline constexpr gpio_num_t kAudioBclk = GPIO_NUM_11;
inline constexpr gpio_num_t kAudioDataIntoCodec = GPIO_NUM_16;
inline constexpr gpio_num_t kAudioDataOutOfCodec = GPIO_NUM_15;
// Both lines are named from the ESP32's side, exactly as the vendor header names
// them for this board (Seeed/xiaozhi: AUDIO_I2S_GPIO_DOUT = GPIO16 into the
// ES8311 DAC, AUDIO_I2S_GPIO_DIN = GPIO15 out of the ES7243E ADC), and as this
// tree's other S3 boards name their kI2sDataOut. Getting the pair the wrong way
// round makes the ESP32 drive the pin the microphone codec drives, so the DAC
// receives nothing while two outputs fight over one line: audible garbage
// instead of the stream being played.
// 0x18 is the ES8311 default address the vendor headers name
// ES8311_CODEC_DEFAULT_ADDR; the ES7243E datasheet default is 0x14.
inline constexpr uint8_t kEs8311Address = 0x18;
inline constexpr uint8_t kEs7243eAddress = 0x14;

// --- IO expander: 16-bit PCA95xx family, switches the board power rails ---
inline constexpr gpio_num_t kExpanderInterrupt = GPIO_NUM_2;
// A2A1A0 strapped to 001, the address the vendor names
// ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_001 (and ..._PCA9535_ADDRESS_001).
// Confirm against a bus scan on the hardware before trusting it.
inline constexpr uint8_t kIoExpanderI2cAddress = 0x21U;
// Expander pin indices use the vendor naming (IO_EXPANDER_PIN_NUM_n), where
// 0-7 are port 0 and 8-15 are port 1.
//
// Direction masks and startup latch levels live in board_power.cpp. Port 0 is
// entirely input and port 1 carries the rail outputs, except P1.5 which is the
// battery-detect input.
inline constexpr uint8_t kExpanderPinChargeDetect = 0;
inline constexpr uint8_t kExpanderPinStandbyDetect = 1;
inline constexpr uint8_t kExpanderPinVbusInDetect = 2;
inline constexpr uint8_t kExpanderPinKnobButton = 3;
inline constexpr uint8_t kExpanderPinSdDetect = 4;
inline constexpr uint8_t kExpanderPinTouchInterrupt = 5;
inline constexpr uint8_t kExpanderPinAiChipSync = 6;
inline constexpr uint8_t kExpanderPinAiChipReset = 7;
inline constexpr uint8_t kExpanderPinPowerSdCard = 8;
inline constexpr uint8_t kExpanderPinPowerLcd = 9;
inline constexpr uint8_t kExpanderPinPowerSystem = 10;
inline constexpr uint8_t kExpanderPinPowerAiChip = 11;
inline constexpr uint8_t kExpanderPinPowerCodecPa = 12;
inline constexpr uint8_t kExpanderPinBatteryDetect = 13;
inline constexpr uint8_t kExpanderPinPowerGrove = 14;
inline constexpr uint8_t kExpanderPinPowerBatteryAdc = 15;

// --- Rotary knob ---
inline constexpr gpio_num_t kKnobPhaseA = GPIO_NUM_41;
inline constexpr gpio_num_t kKnobPhaseB = GPIO_NUM_42;

// --- Battery ---
inline constexpr int kBatteryAdcChannel = 2;  // ADC1 channel 2 is GPIO3.
// Vendor divider: (62 kOhm + 20 kOhm) / 20 kOhm.
inline constexpr float kBatteryVoltageRatio = (62.0F + 20.0F) / 20.0F;

// --- Buttons and indicator ---
inline constexpr gpio_num_t kBootButton = GPIO_NUM_0;
inline constexpr gpio_num_t kStatusLed = GPIO_NUM_40;

}  // namespace micropixel::platform::sensecap_watcher::board
