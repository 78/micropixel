// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "platform/drivers/rtc/rx8130_codec.hpp"

namespace micropixel::platform::drivers {

struct Rx8130Config final {
    uint8_t address{0x32U};
};

// Bounded RX8130CE driver: the calendar registers, the backup-capacitor charge
// control and the voltage-loss flag the board uses to decide whether the stored
// time can be trusted. Time policy (system clock, write-back) stays with the
// Board. No alarm or wakeup-timer support: on this board the RTC interrupt is
// wired to the power-management MCU, not to the SoC.
class Rx8130 final {
   public:
    Rx8130() = default;
    Rx8130(const Rx8130&) = delete;
    Rx8130& operator=(const Rx8130&) = delete;
    ~Rx8130();

    [[nodiscard]] esp_err_t Bind(i2c_master_bus_handle_t bus, Rx8130Config config = {});
    [[nodiscard]] bool available() const { return device_ != nullptr; }

    // One-time bring-up, safe on every boot: clears the control register (TEST
    // and STOP read 0) and enables backup-capacitor charging (the vendor's
    // "initBat": INIEN plus CHGEN, the rechargeable-backup configuration this
    // board's supercap needs).
    [[nodiscard]] esp_err_t Configure();
    // True when the chip reported a supply drop (VLF), which invalidates the
    // stored calendar until it is written again.
    [[nodiscard]] bool VoltageLossDetected();
    [[nodiscard]] esp_err_t ReadCalendar(rx8130::Fields& fields);
    // Stops the counter while the seven calendar registers are written and
    // restarts it afterwards (the datasheet's STOP-bit protocol).
    [[nodiscard]] esp_err_t WriteCalendar(const rx8130::Fields& fields);

   private:
    [[nodiscard]] bool Prepare();
    void Drop(const char* operation, esp_err_t status);
    [[nodiscard]] esp_err_t ReadRegisters(uint8_t address, uint8_t* out, size_t size);
    [[nodiscard]] esp_err_t WriteRegisters(uint8_t address, const uint8_t* data, size_t size);
    [[nodiscard]] esp_err_t UpdateRegister(uint8_t address, uint8_t mask, uint8_t value);

    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    Rx8130Config config_{};
    int64_t next_probe_us_{};
    bool probe_logged_{};
};

}  // namespace micropixel::platform::drivers
