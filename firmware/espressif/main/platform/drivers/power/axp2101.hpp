#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::drivers {

struct Axp2101BatterySample final {
    uint8_t percent{};
    bool battery_present{};
    bool charging{};
    bool discharging{};
    bool external_power_connected{};
};

// Minimal AXP2101 register driver. Board code owns rail-to-peripheral mapping
// and every call after startup is serialized by the shared I2C executor.
class Axp2101 final {
   public:
    Axp2101() = default;
    Axp2101(const Axp2101&) = delete;
    Axp2101& operator=(const Axp2101&) = delete;
    ~Axp2101();

    [[nodiscard]] esp_err_t Attach(i2c_master_bus_handle_t bus, uint8_t address = 0x34U);
    [[nodiscard]] esp_err_t SetLdoVoltage(uint8_t voltage_register, uint8_t enable_mask, int voltage_mv);
    [[nodiscard]] esp_err_t ReadBattery(Axp2101BatterySample& sample);
    [[nodiscard]] esp_err_t PowerOff();

   private:
    [[nodiscard]] esp_err_t ReadRegister(uint8_t reg, uint8_t& value) const;
    [[nodiscard]] esp_err_t WriteRegister(uint8_t reg, uint8_t value) const;

    i2c_master_dev_handle_t device_{};
};

}  // namespace micropixel::platform::drivers
