#ifndef MICROPIXEL_DEVICE_BOARD_INFO_HPP
#define MICROPIXEL_DEVICE_BOARD_INFO_HPP

#include <cstdint>

namespace micropixel::device {

struct DisplaySafeAreaInsets final {
    uint32_t top_pixels{};
    uint32_t right_pixels{};
    uint32_t bottom_pixels{};
    uint32_t left_pixels{};
};

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
    // Axis-aligned content insets in native display pixels. Boards derive
    // these from the panel/cover geometry so Apps do not need board-specific
    // padding for rounded corners or other obscured edge regions.
    DisplaySafeAreaInsets safe_area{};
};

struct BoardInfo final {
    const char* board{"Unknown"};
    const char* host_chip{"Unknown"};
    const char* wifi_coprocessor{"Unknown"};
    const char* touch_controller{"Unknown"};
    DisplayInfo display{};
    // Graphics engines enabled by the current firmware profile, rather than
    // accelerators merely supported by the SoC.
    const char* graphics_acceleration{"CPU only"};
};

}  // namespace micropixel::device

#endif
