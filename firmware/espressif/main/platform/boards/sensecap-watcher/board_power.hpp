// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#ifndef MICROPIXEL_PLATFORM_SENSECAP_WATCHER_BOARD_POWER_HPP
#define MICROPIXEL_PLATFORM_SENSECAP_WATCHER_BOARD_POWER_HPP

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::sensecap_watcher {

// Every peripheral rail on the Watcher is switched by a 16-bit PCA95xx family
// expander on the control I2C bus, so nothing else on the board can be brought
// up until this has run. The expander is driven through its own device handle
// rather than the shared executor because it has to work before any task or
// executor exists, the same way the Claw4 board initializes its expander.
class BoardPower final {
   public:
    [[nodiscard]] esp_err_t Initialize();

    [[nodiscard]] i2c_master_bus_handle_t ControlBus() const { return control_bus_; }
    [[nodiscard]] i2c_master_dev_handle_t Expander() const { return expander_; }

    // VBUS_IN_DET is active low: a low level means external power is present.
    [[nodiscard]] bool IsCharging();

    // Battery detect is also active low.
    [[nodiscard]] bool IsBatteryPresent();

    // Cuts the system rail, which is the vendor's power-off path. Everything
    // else stays latched, so this is only correct as the final action.
    [[nodiscard]] esp_err_t PowerOff();

    // Reads one expander line by its flat pin index, mapping it to the input
    // register and bit the PCA95xx uses. Active levels stay with the caller,
    // because the lines on this expander do not share a polarity. Exposed for
    // inputs that sit on the expander instead of a GPIO, such as the knob's
    // centre switch.
    [[nodiscard]] esp_err_t ReadPinLevel(uint8_t pin, bool& level) const;

   private:
    [[nodiscard]] esp_err_t InitializeControlI2c();
    [[nodiscard]] esp_err_t AddExpanderDevice();
    [[nodiscard]] esp_err_t ConfigureDirections() const;
    [[nodiscard]] esp_err_t SequenceStartup();
    [[nodiscard]] esp_err_t WriteRegister(uint8_t reg, uint8_t value) const;
    [[nodiscard]] esp_err_t ReadRegister(uint8_t reg, uint8_t& value) const;

    i2c_master_bus_handle_t control_bus_{};
    i2c_master_dev_handle_t expander_{};
};

}  // namespace micropixel::platform::sensecap_watcher

#endif  // MICROPIXEL_PLATFORM_SENSECAP_WATCHER_BOARD_POWER_HPP
