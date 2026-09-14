// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace micropixel::platform::esp_mosaico::board {

// Pin numbers are kept independent of ESP-IDF so revision decoding is testable.
struct HardwareConfig {
    const char* name;
    int display_reset;
    int display_clock;
    int i2c_data;
    int i2c_clock;
    int codec_power;
    int status_led;
};

// ESP_EFUSE_USER_DATA starts with (major << 8) | minor, as in the official BSP.
// Unknown/unprogrammed versions must not drive pins belonging to another revision.
[[nodiscard]] constexpr std::optional<HardwareConfig> DecodeHardwareConfig(uint16_t version) {
    switch (version) {
        case 0x0100U:
            return HardwareConfig{"ESP-Mosaico V1.0", 42, 44, 0, 1, 56, 3};
        case 0x0101U:
            return HardwareConfig{"ESP-Mosaico V1.1", 44, 42, 56, 3, -1, -1};
        case 0x0102U:
            return HardwareConfig{"ESP-Mosaico V1.2", 44, 42, 56, 3, -1, -1};
        default:
            return std::nullopt;
    }
}

}  // namespace micropixel::platform::esp_mosaico::board
