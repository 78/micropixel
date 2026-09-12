#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_BOARD_CONFIG_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_BOARD_CONFIG_HPP

#include <array>
#include <cstdint>

#include "device/contracts/peripheral_channel.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace micropixel::platform::esp_mosaico::board {

inline constexpr int32_t kDisplayWidth = 480;
inline constexpr int32_t kDisplayHeight = 480;
inline constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;
inline constexpr gpio_num_t kDisplayReset = GPIO_NUM_42;
inline constexpr gpio_num_t kDisplayTe = GPIO_NUM_43;
inline constexpr gpio_num_t kDisplayChipSelect = GPIO_NUM_50;
inline constexpr gpio_num_t kDisplayClock = GPIO_NUM_44;
inline constexpr gpio_num_t kDisplayData0 = GPIO_NUM_36;
inline constexpr gpio_num_t kDisplayData1 = GPIO_NUM_51;
inline constexpr gpio_num_t kDisplayData2 = GPIO_NUM_35;
inline constexpr gpio_num_t kDisplayData3 = GPIO_NUM_9;
inline constexpr uint32_t kDisplayPixelClockHz = 40U * 1000U * 1000U;
inline constexpr uint32_t kDisplayDataWidth = 4U;
inline constexpr uint32_t kDisplayBitsPerPixel = 16U;

inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_6;
inline constexpr gpio_num_t kI2cData = GPIO_NUM_0;
inline constexpr gpio_num_t kI2cClock = GPIO_NUM_1;
inline constexpr gpio_num_t kStatusLed = GPIO_NUM_3;
inline constexpr gpio_num_t kFunctionButton = GPIO_NUM_7;

inline constexpr gpio_num_t kPeripheralPower = GPIO_NUM_60;
inline constexpr gpio_num_t kPowerSwitch = GPIO_NUM_57;
inline constexpr uint32_t kPowerRampMs = 50U;
inline constexpr uint32_t kLightSleepEntryAttempts = 3U;

inline constexpr gpio_num_t kHapticMotor = GPIO_NUM_8;

// On-board 1 Gbit SPI NAND (GD5F1GM7) on its own GP-SPI host. The pins are the
// SD_* pad group of the CoreBoard; the NAND is the only device on this bus.
inline constexpr spi_host_device_t kNandSpiHost = SPI3_HOST;
inline constexpr gpio_num_t kNandClock = GPIO_NUM_20;
inline constexpr gpio_num_t kNandDataOut = GPIO_NUM_21;  // NAND_D / SIO0
inline constexpr gpio_num_t kNandDataIn = GPIO_NUM_22;   // NAND_Q / SIO1
inline constexpr gpio_num_t kNandChipSelect = GPIO_NUM_23;
inline constexpr gpio_num_t kNandHold = GPIO_NUM_24;          // NAND_HOLD / SIO3
inline constexpr gpio_num_t kNandWriteProtect = GPIO_NUM_25;  // NAND_WP / SIO2
inline constexpr uint32_t kNandClockHz = 40U * 1000U * 1000U;
// BundleFS allocation unit on the NAND App store: two FTL sectors. Halves the
// resident Catalog block map against the 2 KiB sector while wasting at most
// 2 KiB per installed Bundle. Recorded in the Catalog; only fresh formats use it.
inline constexpr uint32_t kNandBundleBlockSize = 4096U;

// Only pins that remain independent of display, touch, shared I2C, audio,
// module EEPROM selection, power control and recovery/debug transports are
// Guest-visible. The hardware can route more pins through the GPIO matrix,
// but those require a future board resource arbiter rather than a static lease.
inline constexpr std::array<device::PeripheralChannelId, 17U> kApplicationGpioLines{
    55U, 19U, 18U, 17U, 16U, 15U, 4U, 12U, 13U, 48U, 53U, 38U, 5U, 10U, 11U, 47U, 46U};

inline constexpr gpio_num_t kAudioBitClock = GPIO_NUM_37;
// The schematic net names use the codec direction. GPIO52 feeds the ES8311
// DSDIN pin, so it is the ESP I2S transmitter output.
inline constexpr gpio_num_t kAudioDataOut = GPIO_NUM_52;
inline constexpr gpio_num_t kAudioAmplifierEnable = GPIO_NUM_45;
inline constexpr gpio_num_t kAudioWordSelect = GPIO_NUM_49;
inline constexpr gpio_num_t kAudioDataIn = GPIO_NUM_40;
inline constexpr gpio_num_t kAudioMasterClock = GPIO_NUM_54;
inline constexpr gpio_num_t kAudioCodecPower = GPIO_NUM_56;
inline constexpr uint8_t kAudioCodecI2cAddress = 0x19U;
// Mix/output rate. 32 kHz keeps the ES8311 MCLK at 256*fs = 8.192 MHz and
// halves the aliasing headroom problem of 16 kHz for Guest PCM; Opus clips
// still decode at 16 kHz and are upsampled 2x by the playback service.
inline constexpr uint32_t kAudioSampleRate = 32000U;

}  // namespace micropixel::platform::esp_mosaico::board

#endif
