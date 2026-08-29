#ifndef MICROPIXEL_DEVICE_BOARD_INFO_HPP
#define MICROPIXEL_DEVICE_BOARD_INFO_HPP

#include <cstdint>

namespace micropixel::device {

// Static board identity and presentation metadata used by Host diagnostics.
// String pointers registered by a Board must remain valid for the lifetime of
// the firmware. Runtime-discovered values such as chip revision and memory
// capacity are intentionally collected by their Host consumers.
struct DisplayInfo final {
    const char* driver{"Unknown"};
    const char* interface{"Unknown"};
    const char* pixel_format{"Unknown"};
    uint32_t width_pixels{};
    uint32_t height_pixels{};
    uint32_t refresh_rate_hz{};
};

struct BoardInfo final {
    const char* board{"Unknown"};
    const char* host_chip{"Unknown"};
    const char* wifi_coprocessor{"Unknown"};
    const char* touch_controller{"Unknown"};
    DisplayInfo display{};
};

}  // namespace micropixel::device

#endif
