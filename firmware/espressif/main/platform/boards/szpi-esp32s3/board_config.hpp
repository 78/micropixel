#pragma once

#include <array>

#include "device/contracts/peripheral_channel.hpp"
#include "driver/gpio.h"

namespace micropixel::platform::szpi_esp32s3::board {

struct ExpansionGpioLine final {
    device::PeripheralChannelId channel;
    const char* name;
};

inline constexpr std::array<device::PeripheralChannelId, 2U> kApplicationGpioLines{10U, 11U};
inline constexpr gpio_num_t kBootButton = GPIO_NUM_0;
inline constexpr std::array<ExpansionGpioLine, 2U> kExpansionGpioLines{{
    {10U, "Multipurpose connector GPIO10"},
    {11U, "Multipurpose connector GPIO11"},
}};

}  // namespace micropixel::platform::szpi_esp32s3::board
