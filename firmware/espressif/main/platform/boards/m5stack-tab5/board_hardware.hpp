// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "platform/boards/m5stack-tab5/io_expander.hpp"

namespace micropixel::platform::m5stack_tab5 {

// Board-lifetime hardware shared by display, touch and (later) power.
// The instance lives in internal RAM because its I2C handles and brightness
// state are touched from the board startup path and the LVGL task.
class BoardHardware final {
   public:
    BoardHardware() = default;
    BoardHardware(const BoardHardware&) = delete;
    BoardHardware& operator=(const BoardHardware&) = delete;

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }
    [[nodiscard]] Pi4ioeExpander& Expander() { return expander_; }
    // Power-up sequence for the panel and the touch controller: pulse both
    // resets and then run the GT911 unlock. The two steps must stay together:
    // the unlock is cleared by any reset, so splitting the call would bring
    // back the "I2C answers but touch never reports" failure.
    [[nodiscard]] esp_err_t ResetPanelAndTouch();
    [[nodiscard]] esp_err_t SetBrightness(int percent);

   private:
    i2c_master_bus_handle_t i2c_bus_{};
    Pi4ioeExpander expander_{};
    bool brightness_ready_{};
};

}  // namespace micropixel::platform::m5stack_tab5
