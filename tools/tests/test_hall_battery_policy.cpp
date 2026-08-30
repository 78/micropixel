#include <cassert>

#include "host/controller/hall_battery_policy.hpp"

int main() {
    using micropixel::firmware::hall_battery_policy::ShowCharging;

    static_assert(ShowCharging(true, true, true, true));
    static_assert(!ShowCharging(true, false, true, true));
    static_assert(!ShowCharging(true, false, true, false));
    static_assert(ShowCharging(false, false, true, true));
    static_assert(!ShowCharging(false, false, false, false));

    assert(!ShowCharging(true, false, true, true));
    return 0;
}
