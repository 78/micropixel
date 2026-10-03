#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "platform/drivers/power/axp2101.hpp"

namespace micropixel::platform::ksdiy_p4c5 {

// Board bring-up through kevincoooool/ksdiy_p4c5_bsp: I2C0, the AXP2101 codec
// rails, the ST7102 MIPI-DSI panel, its backlight and the ST7123 touch handle.
// MicroPixel attaches its own AXP2101 driver only for the battery state; every
// I2C call after startup goes through the board's shared I2C executor.
class BoardIo final {
   public:
    BoardIo() = default;
    BoardIo(const BoardIo&) = delete;
    BoardIo& operator=(const BoardIo&) = delete;

    // Runs before the I2C executor starts, so it may touch the bus directly.
    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SetDisplayEnabled(bool enabled);
    [[nodiscard]] esp_err_t SetBacklightOutputPerTenThousand(uint32_t output);

    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }
    [[nodiscard]] drivers::Axp2101& Pmic() { return pmic_; }
    [[nodiscard]] bool PmicAvailable() const { return pmic_available_; }
    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return panel_; }
    [[nodiscard]] esp_lcd_touch_handle_t Touch() const { return touch_; }

   private:
    void AttachPmic();

    i2c_master_bus_handle_t i2c_bus_{};
    drivers::Axp2101 pmic_{};
    bool pmic_available_{};
    esp_lcd_panel_handle_t panel_{};
    esp_lcd_touch_handle_t touch_{};
};

}  // namespace micropixel::platform::ksdiy_p4c5
