#pragma once

namespace micropixel::firmware::hall_battery_policy {

[[nodiscard]] constexpr bool ShowCharging(bool charging_available, bool charging, bool external_power_available,
                                          bool external_power_connected) {
    return charging_available ? charging : external_power_available && external_power_connected;
}

}  // namespace micropixel::firmware::hall_battery_policy
