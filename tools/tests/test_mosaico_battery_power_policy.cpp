#include <cassert>

#include "platform/boards/esp-mosaico/battery_power_policy.hpp"

int main() {
    using namespace micropixel::platform::esp_mosaico::battery_policy;

    static_assert(IsCharging(6));
    static_assert(!IsCharging(5));
    static_assert(IsDischarging(-6));
    static_assert(!IsDischarging(-5));
    static_assert(!ExternalPowerConnected(-6));
    static_assert(ExternalPowerConnected(-5));
    static_assert(ExternalPowerConnected(0));
    static_assert(ExternalPowerConnected(6));

    assert(ExternalPowerConnected(0));
    return 0;
}
