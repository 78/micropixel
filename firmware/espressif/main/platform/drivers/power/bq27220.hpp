#pragma once

#include <cstdint>

#include "driver/i2c_master.h"

namespace micropixel::platform::drivers {

struct Bq27220Sample final {
    uint16_t voltage_mv{};
    int16_t current_ma{};
};

// Bounded read-only fuel-gauge driver shared by board battery policies. It
// owns probing, the I2C device handle and failure backoff; percentage/filter
// and external-power semantics stay with the board backend.
class Bq27220 final {
   public:
    void Bind(i2c_master_bus_handle_t bus) { bus_ = bus; }
    [[nodiscard]] bool Read(Bq27220Sample& sample, int64_t now_us);
    [[nodiscard]] bool ReadStateOfCharge(uint8_t& percent, int64_t now_us);
    [[nodiscard]] bool available() const { return device_ != nullptr; }

   private:
    [[nodiscard]] bool Attach(int64_t now_us);
    [[nodiscard]] bool ReadRegister(uint8_t address, uint16_t& value, int64_t now_us);

    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    int64_t next_probe_us_{};
    int64_t read_cooldown_until_us_{};
    uint32_t consecutive_read_failures_{};
    bool probe_warning_logged_{};
};

}  // namespace micropixel::platform::drivers
