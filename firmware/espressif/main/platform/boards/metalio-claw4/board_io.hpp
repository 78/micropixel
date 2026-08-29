#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_BOARD_IO_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_BOARD_IO_HPP

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

namespace micropixel::platform::metalio_claw4 {

class BoardIo final {
   public:
    BoardIo(int32_t display_width, int32_t display_height);

    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SuspendDisplay();
    [[nodiscard]] esp_err_t ResumeDisplay();
    [[nodiscard]] esp_err_t SetBacklight(bool enabled);
    [[nodiscard]] esp_err_t SetBacklightOutputPerTenThousand(uint32_t output);

    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }
    [[nodiscard]] i2c_master_dev_handle_t IoExpander() const { return io_expander_; }
    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return panel_; }
    [[nodiscard]] esp_lcd_panel_io_handle_t PanelIo() const { return panel_io_; }

   private:
    [[nodiscard]] esp_err_t InitializeI2c();
    [[nodiscard]] esp_err_t InitializeIoExpander();
    [[nodiscard]] esp_err_t InitializeLcd();

    int32_t display_width_{};
    int32_t display_height_{};
    i2c_master_bus_handle_t i2c_bus_{};
    i2c_master_dev_handle_t io_expander_{};
    esp_lcd_dsi_bus_handle_t dsi_bus_{};
    esp_lcd_panel_handle_t panel_{};
    esp_lcd_panel_io_handle_t panel_io_{};
    esp_ldo_channel_handle_t dsi_ldo_{};
    bool panel_dma2d_enabled_{};
    bool backlight_pwm_configured_{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
