#include "platform/metalio-claw4/board_hardware.hpp"

#include "driver/gpio.h"
#include "esp_lcd_mipi_dsi.h"
#include "platform/metalio-claw4/display/esp_lcd_nv3051f.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr gpio_num_t kLcdReset = GPIO_NUM_3;
constexpr gpio_num_t kI2cSda = GPIO_NUM_7;
constexpr gpio_num_t kI2cScl = GPIO_NUM_8;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_52;
constexpr int kDsiLdoChannel = 3;
constexpr int kDsiLdoMillivolts = 2500;
constexpr uint32_t kDisplayFramebufferCount = 2U;

esp_err_t ConfigureOutput(gpio_num_t pin, int initial_level) {
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t status = gpio_config(&config);
    return status == ESP_OK ? gpio_set_level(pin, initial_level) : status;
}

}  // namespace

BoardHardware::BoardHardware(int32_t display_width, int32_t display_height)
    : display_width_(display_width), display_height_(display_height) {}

esp_err_t BoardHardware::Initialize() {
    esp_err_t status = SetBacklight(false);
    if (status == ESP_OK) {
        status = InitializeI2c();
    }
    if (status == ESP_OK) {
        status = InitializeLcd();
    }
    return status;
}

esp_err_t BoardHardware::SetBacklight(bool enabled) {
    if (!backlight_configured_) {
        esp_err_t status = ConfigureOutput(kLcdBacklight, enabled ? 1 : 0);
        if (status != ESP_OK) {
            return status;
        }
        backlight_configured_ = true;
        return ESP_OK;
    }
    return gpio_set_level(kLcdBacklight, enabled ? 1 : 0);
}

esp_err_t BoardHardware::InitializeI2c() {
    i2c_master_bus_config_t config{};
    config.i2c_port = I2C_NUM_1;
    config.sda_io_num = kI2cSda;
    config.scl_io_num = kI2cScl;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.intr_priority = 0;
    config.trans_queue_depth = 0;
    config.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&config, &i2c_bus_);
}

esp_err_t BoardHardware::InitializeLcd() {
    esp_ldo_channel_config_t ldo_config{};
    ldo_config.chan_id = kDsiLdoChannel;
    ldo_config.voltage_mv = kDsiLdoMillivolts;
    esp_err_t status = esp_ldo_acquire_channel(&ldo_config, &dsi_ldo_);
    if (status != ESP_OK) {
        return status;
    }

    esp_lcd_dsi_bus_config_t bus_config{};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = 1000;
    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    status = esp_lcd_new_dsi_bus(&bus_config, &dsi_bus);
    if (status != ESP_OK) {
        return status;
    }

    const esp_lcd_dbi_io_config_t dbi_config = NV3051F_PANEL_IO_DBI_CONFIG();
    status = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &panel_io_);
    if (status != ESP_OK) {
        return status;
    }

    esp_lcd_dpi_panel_config_t dpi_config{};
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 48;
    dpi_config.virtual_channel = 0;
    dpi_config.num_fbs = kDisplayFramebufferCount;
    dpi_config.video_timing.h_size = display_width_;
    dpi_config.video_timing.v_size = display_height_;
    dpi_config.video_timing.hsync_back_porch = 44;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_front_porch = 46;
    dpi_config.video_timing.vsync_back_porch = 14;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_front_porch = 16;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB888;

    nv3051f_vendor_config_t vendor_config{};
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = kLcdReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 24;
    panel_config.vendor_config = &vendor_config;
    status = esp_lcd_new_panel_nv3051f(panel_io_, &panel_config, &panel_);
    if (status != ESP_OK) {
        return status;
    }
    status = esp_lcd_dpi_panel_enable_dma2d(panel_);
    if (status != ESP_OK) {
        return status;
    }
    status = esp_lcd_panel_reset(panel_);
    if (status != ESP_OK) {
        return status;
    }
    status = esp_lcd_panel_init(panel_);
    if (status != ESP_OK) {
        return status;
    }
    return esp_lcd_panel_disp_on_off(panel_, true);
}

}  // namespace micropixel::platform::metalio_claw4
