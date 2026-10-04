// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace micropixel::platform::m5stack_tab5 {

// Two PI4IOE5V6408 8-bit IO expanders on the system I2C bus (net names from
// the Tab5 schematic).
//   U1 (0x43): EXT_ANT P0, SPK_EN P1, EXT_5V_EN P2, LCD_RST P4, TP_RST P5,
//              CAM_RST P6, headphone detect P7 (input)
//   U2 (0x44): WLAN_PWR_EN P0, USB5V_EN P3, PWR_OFF P4 (pulse), nCHG_QC_EN P5,
//              charger status P6 (input), CHG_EN P7
//
// Both reset lines are driven as plain push-pull outputs (the sequence the
// device ships with). The ILI9881C + GT911 panel needs LCD_RST and TP_RST to
// be held low and then driven high, not released to an input pull-up: the
// vendor switched to a weak pull-up for the 1.8 V ST7123/ST7121 panel and that
// change leaves this panel's touch controller unresponsive.
class Pi4ioeExpander final {
   public:
    Pi4ioeExpander() = default;
    Pi4ioeExpander(const Pi4ioeExpander&) = delete;
    Pi4ioeExpander& operator=(const Pi4ioeExpander&) = delete;

    [[nodiscard]] esp_err_t Initialize(i2c_master_bus_handle_t bus);
    // Vendor power-up reset for the display and touch controller.
    [[nodiscard]] esp_err_t ResetPanelAndTouch();
    // Latches one output bit on the selected expander (0 = U1, 1 = U2).
    [[nodiscard]] esp_err_t SetOutputBit(uint8_t unit, uint8_t bit, bool high);
    // Reads the input port of the selected expander (0 = U1, 1 = U2). Used for
    // the board's active-low USB-C detect line on U2 P6.
    [[nodiscard]] esp_err_t ReadInputPort(uint8_t unit, uint8_t& value);

   private:
    static constexpr size_t kDeviceCount = 2U;

    [[nodiscard]] esp_err_t WriteRegister(uint8_t unit, uint8_t reg, uint8_t value);
    [[nodiscard]] esp_err_t ReadRegister(uint8_t unit, uint8_t reg, uint8_t* value);
    // Read-modify-write of the output latch.
    [[nodiscard]] esp_err_t UpdateOutput(uint8_t unit, uint8_t mask, uint8_t set);

    i2c_master_dev_handle_t devices_[kDeviceCount]{};
};

}  // namespace micropixel::platform::m5stack_tab5
