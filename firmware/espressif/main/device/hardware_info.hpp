#ifndef MICROPIXEL_DEVICE_HARDWARE_INFO_HPP
#define MICROPIXEL_DEVICE_HARDWARE_INFO_HPP

#include <cstdint>

namespace micropixel::device {

// Static board identity and presentation metadata used by Host diagnostics.
// String pointers returned by a backend must remain valid for the lifetime of
// the firmware. Runtime-discovered values such as chip revision and memory
// capacity are intentionally collected by their Host consumers.
struct DisplayHardwareInfo final {
    const char* driver{"Unknown"};
    const char* interface{"Unknown"};
    const char* pixel_format{"Unknown"};
    uint32_t width_pixels{};
    uint32_t height_pixels{};
    uint32_t refresh_rate_hz{};
};

struct HardwareInfo final {
    const char* board{"Unknown"};
    const char* host_chip{"Unknown"};
    const char* wifi_coprocessor{"Unknown"};
    const char* touch_controller{"Unknown"};
    DisplayHardwareInfo display{};
};

class HardwareInfoBackend {
   public:
    virtual ~HardwareInfoBackend() = default;
    HardwareInfoBackend(const HardwareInfoBackend&) = delete;
    HardwareInfoBackend& operator=(const HardwareInfoBackend&) = delete;

    [[nodiscard]] virtual HardwareInfo Snapshot() const = 0;

   protected:
    HardwareInfoBackend() = default;
};

}  // namespace micropixel::device

#endif
