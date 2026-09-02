#pragma once

#include <array>

#include "device/contracts/peripheral_channel.hpp"

namespace micropixel::platform::esp32_s3_box_3::board {

struct PmodGpioLine final {
    device::PeripheralChannelId channel;
    const char* name;
};

// GPIO19/20 remain owned by native USB Serial/JTAG. GPIO40/41 remain owned by
// the BSP's Dock I2C bus. The remaining Pmod pins are independent while the
// currently unsupported SD-card/UART peripheral modes are disabled.
inline constexpr std::array<device::PeripheralChannelId, 12U> kApplicationGpioLines{42U, 39U, 21U, 38U, 13U, 9U,
                                                                                    12U, 44U, 10U, 14U, 11U, 43U};

inline constexpr std::array<PmodGpioLine, 12U> kPmodGpioLines{{
    {42U, "Pmod 1 IO1 (GPIO42)"},
    {39U, "Pmod 1 IO3 (GPIO39)"},
    {21U, "Pmod 1 IO5 (GPIO21)"},
    {38U, "Pmod 1 IO7 (GPIO38)"},
    {13U, "Pmod 2 IO1 (GPIO13)"},
    {9U, "Pmod 2 IO2 (GPIO9)"},
    {12U, "Pmod 2 IO3 (GPIO12)"},
    {44U, "Pmod 2 IO4 (GPIO44)"},
    {10U, "Pmod 2 IO5 (GPIO10)"},
    {14U, "Pmod 2 IO6 (GPIO14)"},
    {11U, "Pmod 2 IO7 (GPIO11)"},
    {43U, "Pmod 2 IO8 (GPIO43)"},
}};

}  // namespace micropixel::platform::esp32_s3_box_3::board
