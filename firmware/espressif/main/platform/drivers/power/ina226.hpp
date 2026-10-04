// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::drivers {

struct Ina226Config final {
    uint8_t address{0x41U};
    // Shunt the bus current flows through, and the current the calibration
    // register is derived from. The vendor BSP of this board uses 5 mOhm with
    // an 8.192 A range, which yields a 250 uA current LSB.
    float shunt_ohms{0.005F};
    float calibration_max_current_amps{8.192F};
};

struct Ina226Sample final {
    float bus_voltage_volts{};
    float shunt_current_amps{};
    float bus_power_watts{};
};

// Bounded INA226 power-monitor driver: probing with backoff, the vendor's
// averaging window and the calibration that turns the shunt reading into
// amperes. Battery semantics (percentage, charging, external power) stay with
// the Board.
class Ina226 final {
   public:
    Ina226() = default;
    Ina226(const Ina226&) = delete;
    Ina226& operator=(const Ina226&) = delete;
    ~Ina226();

    [[nodiscard]] esp_err_t Bind(i2c_master_bus_handle_t bus, Ina226Config config);
    [[nodiscard]] bool Read(Ina226Sample& sample, int64_t now_us);
    [[nodiscard]] bool available() const { return device_ != nullptr; }

   private:
    [[nodiscard]] bool Prepare(int64_t now_us);
    [[nodiscard]] bool ReadRegister(uint8_t address, uint16_t& value);
    [[nodiscard]] bool WriteRegister(uint8_t address, uint16_t value);
    void DropDevice(const char* operation, esp_err_t status, int64_t now_us);

    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    Ina226Config config_{};
    float current_lsb_amps_{0.00025F};
    int64_t next_probe_us_{};
    int64_t read_cooldown_until_us_{};
    bool probe_failed_logged_{};
};

}  // namespace micropixel::platform::drivers
