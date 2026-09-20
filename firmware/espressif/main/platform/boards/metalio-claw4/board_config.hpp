#ifndef MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_BOARD_CONFIG_HPP
#define MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_BOARD_CONFIG_HPP

#include <array>
#include <cstdint>

#include "device/contracts/peripheral_channel.hpp"
#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/ledc.h"
#include "driver/uart.h"

namespace micropixel::platform::metalio_claw4::board {

inline constexpr device::PeripheralChannelId kAccelerationChannel = 1U;
inline constexpr device::PeripheralChannelId kMagneticFieldChannel = 2U;

inline constexpr std::array<device::PeripheralChannelId, 14> kApplicationGpioLines{5U,  14U, 15U, 16U, 17U, 18U, 19U,
                                                                                   20U, 21U, 23U, 35U, 46U, 47U, 48U};

inline constexpr int32_t kDisplayWidth = 720;
inline constexpr int32_t kDisplayHeight = 720;
inline constexpr gpio_num_t kDisplayReset = GPIO_NUM_3;
inline constexpr gpio_num_t kDisplayBacklight = GPIO_NUM_52;
inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_33;
inline constexpr int kDsiLdoChannel = 3;
inline constexpr int kDsiLdoMillivolts = 2500;

inline constexpr i2c_port_t kI2cPort = I2C_NUM_1;
inline constexpr gpio_num_t kI2cData = GPIO_NUM_7;
inline constexpr gpio_num_t kI2cClock = GPIO_NUM_8;
inline constexpr uint8_t kIoExpanderI2cAddress = 0x20U;
inline constexpr gpio_num_t kIoExpanderInterrupt = GPIO_NUM_2;

inline constexpr ledc_mode_t kBacklightLedcMode = LEDC_LOW_SPEED_MODE;
inline constexpr ledc_timer_t kBacklightLedcTimer = LEDC_TIMER_0;
inline constexpr ledc_channel_t kBacklightLedcChannel = LEDC_CHANNEL_0;

inline constexpr gpio_num_t kHapticMotor = GPIO_NUM_22;
inline constexpr ledc_mode_t kHapticLedcMode = LEDC_LOW_SPEED_MODE;
inline constexpr ledc_timer_t kHapticLedcTimer = LEDC_TIMER_1;
inline constexpr ledc_channel_t kHapticLedcChannel = LEDC_CHANNEL_1;

inline constexpr uart_port_t kAudioControlUart = UART_NUM_2;
inline constexpr gpio_num_t kAudioControlTx = GPIO_NUM_26;
inline constexpr gpio_num_t kAudioControlRx = GPIO_NUM_27;
inline constexpr int kAudioI2sPort = 0;
inline constexpr gpio_num_t kAudioBitClock = GPIO_NUM_12;
inline constexpr gpio_num_t kAudioWordSelect = GPIO_NUM_10;
inline constexpr gpio_num_t kAudioDataOut = GPIO_NUM_9;
// Mix/output rate of the BT audio bridge.
inline constexpr uint32_t kAudioSampleRate = 16000U;

inline constexpr uart_port_t kCellularUart = UART_NUM_1;
inline constexpr int kCellularBaud = 2000000;
inline constexpr gpio_num_t kCellularTx = GPIO_NUM_28;
inline constexpr gpio_num_t kCellularRx = GPIO_NUM_29;
inline constexpr gpio_num_t kCellularMrdy = GPIO_NUM_13;
inline constexpr gpio_num_t kCellularSrdy = GPIO_NUM_4;

}  // namespace micropixel::platform::metalio_claw4::board

#endif
