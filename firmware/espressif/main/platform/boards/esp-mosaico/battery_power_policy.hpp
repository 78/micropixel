#pragma once

#include <cstdint>

namespace micropixel::platform::esp_mosaico::battery_policy {

inline constexpr int16_t kCurrentDirectionThresholdMa = 5;

[[nodiscard]] constexpr bool IsCharging(int16_t current_ma, bool full_charged) {
    return !full_charged && current_ma > kCurrentDirectionThresholdMa;
}

[[nodiscard]] constexpr bool IsDischarging(int16_t current_ma) { return current_ma < -kCurrentDirectionThresholdMa; }

// CoreBoard V1.0 leaves the HUSB320 I2C/status outputs and TP4057 status
// outputs unconnected to the MCU. During active firmware operation, a clear
// BQ27220 discharge therefore means battery-only operation; external USB/VIN
// power leaves the cell charging or idle once full.
[[nodiscard]] constexpr bool ExternalPowerConnected(int16_t current_ma) { return !IsDischarging(current_ma); }

}  // namespace micropixel::platform::esp_mosaico::battery_policy
