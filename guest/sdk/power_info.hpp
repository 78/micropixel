#ifndef MICROPIXEL_SDK_POWER_INFO_HPP
#define MICROPIXEL_SDK_POWER_INFO_HPP

#include <stdint.h>

#include "sdk/devices.hpp"
#include "sdk/result.hpp"

namespace micropixel {

class Application;

enum class PowerSource : uint16_t {
    kUnknown = 0,
    kBattery = 1,
    kExternal = 2,
};

struct PowerState final {
    DeviceId id{};
    PowerSource source{PowerSource::kUnknown};
    uint8_t battery_percent{};
    bool has_battery{};
    bool charging{};
    bool discharging{};
    bool external_connected{};
};

class PowerInfo final {
   public:
    [[nodiscard]] Result<PowerState> Get(DeviceId device) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };
    explicit constexpr PowerInfo(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
