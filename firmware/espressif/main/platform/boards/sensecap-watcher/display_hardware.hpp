// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

namespace micropixel::platform::sensecap_watcher {

// Owns the QSPI panel bus, the SPD2010 panel handle and the backlight PWM.
//
// The panel has neither a reset line nor a command/data line: SPD2010 carries
// its own GRAM and is driven entirely through the 32-bit command framing the
// vendor driver uses, so the chip selects itself once the panel IO is created.
class DisplayHardware final {
   public:
    DisplayHardware() = default;
    DisplayHardware(const DisplayHardware&) = delete;
    DisplayHardware& operator=(const DisplayHardware&) = delete;

    [[nodiscard]] esp_err_t Initialize();
    // Brightness is PWM duty, so a panel that is off is a zero duty rather
    // than a powered-down rail; the expander owns the LCD rail itself.
    [[nodiscard]] esp_err_t SetBrightness(int percent);

    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return panel_; }
    [[nodiscard]] esp_lcd_panel_io_handle_t PanelIo() const { return panel_io_; }

   private:
    esp_lcd_panel_io_handle_t panel_io_{};
    esp_lcd_panel_handle_t panel_{};
    bool backlight_initialized_{};
};

}  // namespace micropixel::platform::sensecap_watcher
