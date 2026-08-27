#include "platform/boards/metalio-claw4/board_hardware.hpp"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_mipi_dsi.h"
#include "platform/boards/metalio-claw4/perceptual_control.hpp"
#include "platform/drivers/display/nv3051f/esp_lcd_nv3051f.h"

namespace micropixel::platform::metalio_claw4 {
namespace {

constexpr gpio_num_t kLcdReset = GPIO_NUM_3;
constexpr gpio_num_t kI2cSda = GPIO_NUM_7;
constexpr gpio_num_t kI2cScl = GPIO_NUM_8;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_52;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kBacklightPwmFrequencyHz = 20000U;
constexpr uint32_t kBacklightMaxDuty = (1U << 10U) - 1U;
constexpr uint8_t kTca9555Address = 0x20U;
constexpr int kDsiLdoChannel = 3;
constexpr int kDsiLdoMillivolts = 2500;
constexpr uint32_t kDisplayFramebufferCount = 2U;

}  // namespace

BoardHardware::BoardHardware(int32_t display_width, int32_t display_height)
    : display_width_(display_width), display_height_(display_height) {}

esp_err_t BoardHardware::Initialize() {
    esp_err_t status = SetBacklight(false);
    if (status == ESP_OK) {
        status = InitializeI2c();
    }
    if (status == ESP_OK) {
        status = InitializeIoExpander();
    }
    if (status == ESP_OK) {
        status = InitializeLcd();
    }
    return status;
}

esp_err_t BoardHardware::SuspendDisplay() {
    esp_err_t status = SetBacklight(false);
    if (panel_ != nullptr) {
        const esp_err_t display_status = esp_lcd_panel_disp_on_off(panel_, false);
        if (status == ESP_OK) {
            status = display_status;
        }
        if (panel_dma2d_enabled_) {
            const esp_err_t dma2d_status = esp_lcd_dpi_panel_disable_dma2d(panel_);
            if (dma2d_status == ESP_OK) {
                panel_dma2d_enabled_ = false;
            }
            if (status == ESP_OK) {
                status = dma2d_status;
            }
        }
        if (!panel_dma2d_enabled_) {
            const esp_err_t delete_status = esp_lcd_panel_del(panel_);
            if (delete_status == ESP_OK) {
                panel_ = nullptr;
            }
            if (status == ESP_OK) {
                status = delete_status;
            }
        }
    }
    if (panel_ == nullptr && panel_io_ != nullptr) {
        const esp_err_t delete_status = esp_lcd_panel_io_del(panel_io_);
        if (delete_status == ESP_OK) {
            panel_io_ = nullptr;
        }
        if (status == ESP_OK) {
            status = delete_status;
        }
    }
    if (panel_ == nullptr && panel_io_ == nullptr && dsi_bus_ != nullptr) {
        const esp_err_t delete_status = esp_lcd_del_dsi_bus(dsi_bus_);
        if (delete_status == ESP_OK) {
            dsi_bus_ = nullptr;
        }
        if (status == ESP_OK) {
            status = delete_status;
        }
    }
    return status;
}

esp_err_t BoardHardware::ResumeDisplay() {
    if (panel_ != nullptr) {
        if (!panel_dma2d_enabled_) {
            const esp_err_t status = esp_lcd_dpi_panel_enable_dma2d(panel_);
            if (status != ESP_OK) {
                return status;
            }
            panel_dma2d_enabled_ = true;
        }
        return esp_lcd_panel_disp_on_off(panel_, true);
    }
    return InitializeLcd();
}

esp_err_t BoardHardware::SetBacklight(bool enabled) {
    return SetBacklightOutputPerTenThousand(enabled ? kPerceptualControlScale : 0U);
}

esp_err_t BoardHardware::SetBacklightOutputPerTenThousand(uint32_t output) {
    const uint32_t clamped_output = output <= kPerceptualControlScale ? output : kPerceptualControlScale;
    uint32_t duty = (kBacklightMaxDuty * clamped_output + kPerceptualControlScale / 2U) / kPerceptualControlScale;
    if (clamped_output != 0U && duty == 0U) {
        duty = 1U;
    }
    if (!backlight_pwm_configured_) {
        ledc_timer_config_t timer_config{};
        timer_config.speed_mode = kBacklightSpeedMode;
        timer_config.duty_resolution = kBacklightResolution;
        timer_config.timer_num = kBacklightTimer;
        timer_config.freq_hz = kBacklightPwmFrequencyHz;
        timer_config.clk_cfg = LEDC_AUTO_CLK;
        esp_err_t status = ledc_timer_config(&timer_config);
        if (status != ESP_OK) {
            return status;
        }

        ledc_channel_config_t channel_config{};
        channel_config.gpio_num = kLcdBacklight;
        channel_config.speed_mode = kBacklightSpeedMode;
        channel_config.channel = kBacklightChannel;
        channel_config.timer_sel = kBacklightTimer;
        channel_config.duty = duty;
        channel_config.hpoint = 0U;
        status = ledc_channel_config(&channel_config);
        if (status != ESP_OK) {
            return status;
        }
        backlight_pwm_configured_ = true;
        return status;
    }
    esp_err_t status = ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty);
    return status == ESP_OK ? ledc_update_duty(kBacklightSpeedMode, kBacklightChannel) : status;
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

esp_err_t BoardHardware::InitializeIoExpander() {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = kTca9555Address;
    config.scl_speed_hz = 400000U;
    return i2c_master_bus_add_device(i2c_bus_, &config, &io_expander_);
}

esp_err_t BoardHardware::InitializeLcd() {
    if (panel_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dsi_ldo_ == nullptr) {
        esp_ldo_channel_config_t ldo_config{};
        ldo_config.chan_id = kDsiLdoChannel;
        ldo_config.voltage_mv = kDsiLdoMillivolts;
        esp_err_t status = esp_ldo_acquire_channel(&ldo_config, &dsi_ldo_);
        if (status != ESP_OK) {
            return status;
        }
    }

    bool created_bus = false;
    if (dsi_bus_ == nullptr) {
        esp_lcd_dsi_bus_config_t bus_config{};
        bus_config.bus_id = 0;
        bus_config.num_data_lanes = 2;
        bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
        bus_config.lane_bit_rate_mbps = 1000;
        const esp_err_t status = esp_lcd_new_dsi_bus(&bus_config, &dsi_bus_);
        if (status != ESP_OK) {
            return status;
        }
        created_bus = true;
    }

    bool created_panel_io = false;
    if (panel_io_ == nullptr) {
        const esp_lcd_dbi_io_config_t dbi_config = NV3051F_PANEL_IO_DBI_CONFIG();
        const esp_err_t status = esp_lcd_new_panel_io_dbi(dsi_bus_, &dbi_config, &panel_io_);
        if (status != ESP_OK) {
            if (created_bus) {
                (void)esp_lcd_del_dsi_bus(dsi_bus_);
                dsi_bus_ = nullptr;
            }
            return status;
        }
        created_panel_io = true;
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
    vendor_config.mipi_config.dsi_bus = dsi_bus_;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = kLcdReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 24;
    panel_config.vendor_config = &vendor_config;
    esp_err_t status = esp_lcd_new_panel_nv3051f(panel_io_, &panel_config, &panel_);
    if (status != ESP_OK) {
        if (created_panel_io) {
            (void)esp_lcd_panel_io_del(panel_io_);
            panel_io_ = nullptr;
        }
        if (created_bus) {
            (void)esp_lcd_del_dsi_bus(dsi_bus_);
            dsi_bus_ = nullptr;
        }
        return status;
    }
    status = esp_lcd_dpi_panel_enable_dma2d(panel_);
    if (status != ESP_OK) {
        (void)SuspendDisplay();
        return status;
    }
    panel_dma2d_enabled_ = true;
    status = esp_lcd_panel_reset(panel_);
    if (status != ESP_OK) {
        (void)SuspendDisplay();
        return status;
    }
    status = esp_lcd_panel_init(panel_);
    if (status != ESP_OK) {
        (void)SuspendDisplay();
        return status;
    }
    status = esp_lcd_panel_disp_on_off(panel_, true);
    if (status != ESP_OK) {
        (void)SuspendDisplay();
    }
    return status;
}

}  // namespace micropixel::platform::metalio_claw4
