#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_BOARD_CONFIG_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_BOARD_CONFIG_HPP

#include <cstdint>

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

inline constexpr gpio_num_t kPeripheralPower = GPIO_NUM_60;
inline constexpr gpio_num_t kPowerSwitch = GPIO_NUM_57;
inline constexpr uint32_t kPowerRampMs = 50U;

inline constexpr gpio_num_t kHapticMotor = GPIO_NUM_8;

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

}  // namespace micropixel::platform::esp_mosaico::board

#endif
