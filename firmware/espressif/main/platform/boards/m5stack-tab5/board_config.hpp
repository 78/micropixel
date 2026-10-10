// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

#include "device/contracts/peripheral_channel.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

namespace micropixel::platform::m5stack_tab5::board {

// System I2C (I2C port 0) carries the touch controller, the audio codec, both
// PI4IOE5V6416 IO expanders, the RTC and the INA226 power monitor.
inline constexpr i2c_port_num_t kSystemI2cPort = I2C_NUM_0;
inline constexpr gpio_num_t kSystemI2cScl = GPIO_NUM_32;
inline constexpr gpio_num_t kSystemI2cSda = GPIO_NUM_31;
inline constexpr uint32_t kSystemI2cFrequencyHz = 400'000;

// Panel revisions pair a touch controller with their panel driver. The vendor
// BSP probes these addresses first; the result also selects the display driver.
inline constexpr uint8_t kGt911PrimaryAddress = 0x5D;
inline constexpr uint8_t kGt911BackupAddress = 0x14;
inline constexpr uint16_t kSt7123TouchAddress = 0x55;

// Both IO expanders sit on the system I2C bus. The first one owns LCD_RST,
// TP_RST, EXT_5V and headphone detect; the second one owns WLAN_PWR_EN,
// USB5V_EN, the power-off pulse and the charge enables.
inline constexpr uint8_t kIoExpander1Address = 0x43;
inline constexpr uint8_t kIoExpander2Address = 0x44;
// Speaker amplifier enable on expander 1 (the vendor BSP latches P1 high, which
// keeps the amplifier live from the first boot moment).
inline constexpr uint8_t kSpeakerAmplifierUnit = 0U;
inline constexpr uint8_t kSpeakerAmplifierBit = 1U;

// Display: the panel scans out its native 720x1280 frame, which is also the
// logical canvas this board presents (no rotation pipeline). 2 MIPI-DSI data
// lanes, DSI PHY powered from LDO channel 3 at 2.5 V.
inline constexpr int kDisplayWidth = 720;
inline constexpr int kDisplayHeight = 1280;
inline constexpr gpio_num_t kDisplayBacklight = GPIO_NUM_22;
inline constexpr int kDsiLdoChannel = 3;
inline constexpr int kDsiLdoMillivolts = 2500;

// I2S audio: ES8388 speaker DAC + ES7210 microphone ADC.
inline constexpr gpio_num_t kAudioMclk = GPIO_NUM_30;
inline constexpr gpio_num_t kAudioBitClock = GPIO_NUM_27;
inline constexpr gpio_num_t kAudioWordSelect = GPIO_NUM_29;
inline constexpr gpio_num_t kAudioDataOut = GPIO_NUM_26;
inline constexpr gpio_num_t kAudioDataIn = GPIO_NUM_28;

// Touch interrupt (the reset line is driven through IO expander 1).
inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_23;

// BMI270 6-axis IMU on the shared I2C bus. The SDO strap on this board selects
// 7-bit 0x68 (the vendor BSP uses the same address), while the shared driver
// defaults to 0x69 for the boards that strap it high.
inline constexpr uint8_t kInertialAddress = 0x68U;

// INA226 power monitor on the pack input. The vendor BSP reads the battery side
// through it with a 5 mOhm shunt and an 8.192 A range (see the driver default).
inline constexpr uint8_t kPowerMonitorAddress = 0x41U;
// IP2326 charge-status output ("CHG_STAT") on IO expander 2 P6, read through
// the expander's input-status register. The vendor BSP calls this bit a USB-C
// detect, but the schematic maps U7 as P0 WLAN_PWR_EN, P3 USB5V_EN,
// P4 PWROFF_PLUSE, P5 nCHG_QC_EN, P6 CHG_STAT, P7 CHG_EN, so the bit only says
// the charger is active - and it pulses while no pack is fitted (see
// BatteryPeripheral, which derives external power from the INA226 instead).
inline constexpr uint8_t kChargerStatusExpanderUnit = 1U;
inline constexpr uint8_t kChargerStatusExpanderBit = 6U;

// RX8130CE real-time clock on the system I2C bus, kept in UTC so it matches
// the system clock SNTP updates. Its interrupt is wired to the power-management
// MCU (E_TRG), not to the SoC, so the firmware only reads and writes the
// calendar; the board's supercap keeps it running across a power-off.
inline constexpr uint8_t kRtcAddress = 0x32U;

// microSD socket as the external App storage medium, on the SDMMC host the
// vendor BSP uses: slot 0 in 4-bit mode, the six dedicated pins, card IO on
// the P4's on-chip LDO channel 4 (LDO_VO4, 3.3 V). The socket has no card
// detect pin, so a card is only noticed at boot.
inline constexpr int kSdCardLdoChannel = 4;
inline constexpr int kSdCardBusWidth = 4;
inline constexpr gpio_num_t kSdCardClock = GPIO_NUM_43;
inline constexpr gpio_num_t kSdCardCommand = GPIO_NUM_44;
inline constexpr gpio_num_t kSdCardData0 = GPIO_NUM_39;
inline constexpr gpio_num_t kSdCardData1 = GPIO_NUM_40;
inline constexpr gpio_num_t kSdCardData2 = GPIO_NUM_41;
inline constexpr gpio_num_t kSdCardData3 = GPIO_NUM_42;

// Panel transport: two MIPI-DSI lanes at 730 Mbps with the vendor BSP's video
// timing for the native 720x1280 scanout at a 60 MHz DPI pixel clock.
inline constexpr int kDsiLaneBitRateMbps = 730;
inline constexpr float kDpiClockMHz = 60.0F;
inline constexpr int kPanelHsyncBackPorch = 140;
inline constexpr int kPanelHsyncPulseWidth = 40;
inline constexpr int kPanelHsyncFrontPorch = 40;
inline constexpr int kPanelVsyncBackPorch = 20;
inline constexpr int kPanelVsyncPulseWidth = 4;
inline constexpr int kPanelVsyncFrontPorch = 20;
// DOUBLE_DIRECT matches the other ESP32-P4 board: the adapter keeps one
// framebuffer as the displayed image and one as the LVGL draw target, which is
// exactly what this tear-avoid mode requires.
inline constexpr int kDisplayFramebufferCount = 2;

// Backlight: LEDC PWM on GPIO22. Timer 1 / channel 0 stay clear of the two
// application PWM slots (timers/channels 2 and 3).
inline constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
inline constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_1;
inline constexpr uint32_t kBacklightMaximumDuty = (1U << 10U) - 1U;

// Guest-facing expansion lines. M5-Bus carries 18 free GPIOs; its PIN17/PIN18
// are the Host system I2C pair and the remaining pins are power, ground or RST.
// ExtPort1 adds G49/G50. PORT.A's G53/G54 and ExtPort1's G0/G1 stay reserved for
// an I2C accessory bus, and ExtPort2's G31/G32 plus the RS485 G20/G21/G34 stay
// reserved for the Host I2C bus and a future serial service, so none of them
// become Guest GPIO lines (the same policy Mosaico applies to its expansion
// port). GPIO35/37/38 are SoC strapping pins; they stay exposed because the
// M5-Bus header carries them, and their display names say so.
struct ApplicationGpioLine final {
    device::PeripheralChannelId channel;
    const char* name;
};

inline constexpr std::array<device::PeripheralChannelId, 20U> kApplicationGpioLines{
    16U, 17U, 18U, 45U, 19U, 52U, 5U, 38U, 37U, 7U, 6U, 3U, 4U, 2U, 48U, 47U, 35U, 51U, 49U, 50U};

inline constexpr std::array<ApplicationGpioLine, 20U> kApplicationGpioLineNames{{
    {16U, "M5-Bus PIN2 (GPIO16)"},
    {17U, "M5-Bus PIN4 (GPIO17)"},
    {18U, "M5-Bus PIN7 (GPIO18)"},
    {45U, "M5-Bus PIN8 (GPIO45)"},
    {19U, "M5-Bus PIN9 (GPIO19)"},
    {52U, "M5-Bus PIN10 (GPIO52)"},
    {5U, "M5-Bus PIN11 (GPIO5)"},
    {38U, "M5-Bus PIN13 (GPIO38, strap, RXD0)"},
    {37U, "M5-Bus PIN14 (GPIO37, strap, TXD0)"},
    {7U, "M5-Bus PIN15 (GPIO7)"},
    {6U, "M5-Bus PIN16 (GPIO6)"},
    {3U, "M5-Bus PIN19 (GPIO3)"},
    {4U, "M5-Bus PIN20 (GPIO4)"},
    {2U, "M5-Bus PIN21 (GPIO2)"},
    {48U, "M5-Bus PIN22 (GPIO48)"},
    {47U, "M5-Bus PIN23 (GPIO47)"},
    {35U, "M5-Bus PIN24 (GPIO35, strap)"},
    {51U, "M5-Bus PIN26 (GPIO51)"},
    {49U, "ExtPort1 G49 (GPIO49)"},
    {50U, "ExtPort1 G50 (GPIO50)"},
}};

}  // namespace micropixel::platform::m5stack_tab5::board
