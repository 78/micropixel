#include <cassert>

#include "platform/boards/esp-mosaico/battery_power_policy.hpp"
#include "platform/boards/esp-mosaico/battery_profile.hpp"

int main() {
    namespace battery_profile = micropixel::platform::esp_mosaico::battery_profile;
    namespace drivers = micropixel::platform::drivers;
    using namespace micropixel::platform::esp_mosaico::battery_policy;

    static_assert(IsCharging(6, false));
    static_assert(!IsCharging(6, true));
    static_assert(!IsCharging(5, false));
    static_assert(IsDischarging(-6));
    static_assert(!IsDischarging(-5));
    static_assert(!ExternalPowerConnected(-6));
    static_assert(ExternalPowerConnected(-5));
    static_assert(ExternalPowerConnected(0));
    static_assert(ExternalPowerConnected(6));
    static_assert(battery_profile::kProfile.design_capacity_mah == 80U);
    static_assert(battery_profile::kParameters.size() == 26U);
    static_assert(battery_profile::kParameters[4].width == drivers::Bq27220DataWidth::kU8);
    constexpr std::array<uint8_t, 4U> profile_write{0x9fU, 0x92U, 0x00U, 0x50U};
    static_assert(drivers::Bq27220Checksum(profile_write) == 0x7eU);

    assert(ExternalPowerConnected(0));
    return 0;
}
