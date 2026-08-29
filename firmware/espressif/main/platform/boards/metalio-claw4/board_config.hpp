#ifndef MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_BOARD_CONFIG_HPP
#define MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_BOARD_CONFIG_HPP

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/ledc.h"
#include "driver/uart.h"

namespace micropixel::platform::metalio_claw4::board {

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

}  // namespace micropixel::platform::metalio_claw4::board

#endif
