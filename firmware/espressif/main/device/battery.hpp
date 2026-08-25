#ifndef MICROPIXEL_DEVICE_BATTERY_HPP
#define MICROPIXEL_DEVICE_BATTERY_HPP

#include <cstdint>

namespace micropixel::device {

struct BatterySnapshot final {
    uint8_t percent{};
    bool available{};
    bool charging{};
    bool discharging{};
    bool charging_available{};
    bool external_power_connected{};
    bool external_power_available{};
};

// Invoked from the backend's device-event context after its snapshot may have
// changed. The sink must be non-blocking and must not call back into the
// BatteryBackend.
using BatteryStateChangeSink = void (*)(void* context);

class BatteryBackend {
   public:
    virtual ~BatteryBackend() = default;
    BatteryBackend(const BatteryBackend&) = delete;
    BatteryBackend& operator=(const BatteryBackend&) = delete;

    [[nodiscard]] virtual BatterySnapshot Snapshot() = 0;
    virtual void SetStateChangeSink(BatteryStateChangeSink sink, void* context) = 0;

   protected:
    BatteryBackend() = default;
};

}  // namespace micropixel::device

#endif
