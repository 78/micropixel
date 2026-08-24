#ifndef MICROPIXEL_DEVICE_BATTERY_HPP
#define MICROPIXEL_DEVICE_BATTERY_HPP

#include <cstdint>

namespace micropixel::device {

struct BatterySnapshot final {
    uint8_t percent{};
    bool available{};
};

class BatteryBackend {
   public:
    virtual ~BatteryBackend() = default;
    BatteryBackend(const BatteryBackend&) = delete;
    BatteryBackend& operator=(const BatteryBackend&) = delete;

    [[nodiscard]] virtual BatterySnapshot Snapshot() = 0;

   protected:
    BatteryBackend() = default;
};

}  // namespace micropixel::device

#endif
