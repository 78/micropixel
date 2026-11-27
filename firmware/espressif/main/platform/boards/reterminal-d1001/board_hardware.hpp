// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"

namespace micropixel::platform::reterminal_d1001 {

// Board-lifetime hardware shared by the display, touch and power paths: the
// system I2C bus, the PCA9535 rail expander and the LEDC backlight. The
// instance lives in internal RAM because its I2C handles and brightness state
// are touched from the board startup path and the LVGL task.
class BoardHardware final {
   public:
    BoardHardware() = default;
    BoardHardware(const BoardHardware&) = delete;
    BoardHardware& operator=(const BoardHardware&) = delete;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }
    [[nodiscard]] esp_io_expander_handle_t Expander() const { return expander_; }
    // Vendor power-up reset for the panel and the touch controller. The panel
    // reset is held low for 10 ms and released for 120 ms; the touch reset is
    // pulsed again by the GSL3670 driver during its own initialization.
    [[nodiscard]] esp_err_t ResetPanelAndTouch();
    [[nodiscard]] esp_err_t SetBrightness(int percent);
    // Drives the expander touch-reset line. `asserted` means the controller is
    // held in reset (the line is active-low).
    [[nodiscard]] esp_err_t SetTouchReset(bool asserted);
    // Speaker amplifier enable on PCA9535 P1.3.
    [[nodiscard]] esp_err_t SetSpeakerAmplifier(bool enabled);
    // Releases the PMIC rail latch. Only the power-off path calls this.
    [[nodiscard]] esp_err_t SetPowerHold(bool enabled);

   private:
    [[nodiscard]] esp_err_t ConfigureRails();
    [[nodiscard]] esp_err_t SetExpanderLevel(uint32_t mask, bool high);

    i2c_master_bus_handle_t i2c_bus_{};
    esp_io_expander_handle_t expander_{};
    bool brightness_ready_{};
};

}  // namespace micropixel::platform::reterminal_d1001