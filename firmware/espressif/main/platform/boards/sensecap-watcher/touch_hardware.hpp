// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

namespace micropixel::platform::sensecap_watcher {

// The touch controller is SPD2010, the companion part to the panel, on its own
// I2C bus. Its interrupt is not a GPIO: the vendor routes it through the IO
// expander, so this board does not register the controller's interrupt callback
// and the input adapter polls it instead.
class TouchHardware final {
   public:
    TouchHardware() = default;
    TouchHardware(const TouchHardware&) = delete;
    TouchHardware& operator=(const TouchHardware&) = delete;

    [[nodiscard]] esp_err_t Initialize();

    [[nodiscard]] esp_lcd_touch_handle_t Touch() const { return touch_; }

   private:
    i2c_master_bus_handle_t bus_{};
    esp_lcd_panel_io_handle_t io_{};
    esp_lcd_touch_handle_t touch_{};
};

}  // namespace micropixel::platform::sensecap_watcher
