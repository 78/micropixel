// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/display_hardware.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_spd2010.h"
#include "esp_log.h"
#include "platform/boards/sensecap-watcher/board_config.hpp"

// The panel is an SPD2010 with its own GRAM, driven over QSPI. Its driver comes
// from the esp-iot-solution tree this repository already vendors, so the
// framing here is the component's own configuration with only the values this
// board changes applied on top.
namespace micropixel::platform::sensecap_watcher {
namespace {

constexpr char kTag[] = "sensecap_watcher_display";

constexpr spi_host_device_t kLcdSpiHost = SPI3_HOST;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_1;
constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_10_BIT;
constexpr uint32_t kBacklightFrequencyHz = 5000U;
constexpr uint32_t kBacklightMaximumDuty = (1U << 10U) - 1U;

// One flush is a partial band. The area rounder widens it to the next 4 pixel
// boundary the panel requires, so the bus accepts one band plus that step.
constexpr size_t kLcdTransferBytes =
    (static_cast<size_t>(board::kDisplayWidth) + 4U) * CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT * 2U;

// The vendor drives every line on the panel connector low once before it
// installs the bus, chip select included. Without that the panel does not
// answer on QSPI.
//
// Two pins are deliberately left out. The backlight is owned by LEDC, and the
// touch pair is owned by its own I2C master; configuring either as a plain GPIO
// output first makes the peripheral that owns it report the pin as unusable.
// The states the vendor's low levels produce are equivalent: a zero duty keeps
// the backlight off, and the I2C master drives both lines itself.
esp_err_t PreparePanelPins() {
    constexpr gpio_num_t kPins[] = {
        board::kLcdPclk, board::kLcdData0, board::kLcdData1, board::kLcdData2, board::kLcdData3, board::kLcdCs,
    };
    gpio_config_t config{};
    for (const gpio_num_t pin : kPins) {
        config.pin_bit_mask |= 1ULL << static_cast<uint32_t>(pin);
    }
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&config), kTag, "precondition panel pins failed");
    for (const gpio_num_t pin : kPins) {
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag, "precondition panel pins failed");
    }
    return ESP_OK;
}

esp_err_t InitializeBacklight() {
    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = kBacklightResolution;
    timer_config.timer_num = kBacklightTimer;
    timer_config.freq_hz = kBacklightFrequencyHz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag, "configure backlight timer failed");

    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = board::kLcdBacklight;
    channel_config.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_config.channel = kBacklightChannel;
    channel_config.timer_sel = kBacklightTimer;
    channel_config.duty = 0U;
    channel_config.hpoint = 0;
    // The vendor lights this panel with a high level, so a backlight that is
    // active high is not inverted.
    channel_config.flags.output_invert = board::kLcdBacklightActiveHigh ? 0 : 1;
    return ledc_channel_config(&channel_config);
}

}  // namespace

esp_err_t DisplayHardware::Initialize() {
    if (panel_io_ != nullptr || panel_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(PreparePanelPins(), kTag, "precondition panel pins failed");
    ESP_RETURN_ON_ERROR(InitializeBacklight(), kTag, "configure backlight failed");
    backlight_initialized_ = true;

    // The component's own QSPI macros are C-only: they use designated
    // initializers whose order does not match spi_bus_config_t and leave the
    // remaining members unset, which this repository compiles as errors. Every
    // field is set explicitly instead, the way the other boards do it.
    //
    // In spi_bus_config_t the QSPI data lines are unions with the single-line
    // names: data0 is mosi, data1 is miso, data2 is quadwp and data3 is quadhd.
    spi_bus_config_t bus_config{};
    bus_config.data0_io_num = board::kLcdData0;
    bus_config.data1_io_num = board::kLcdData1;
    bus_config.sclk_io_num = board::kLcdPclk;
    bus_config.data2_io_num = board::kLcdData2;
    bus_config.data3_io_num = board::kLcdData3;
    // The unused octal lines must be -1. Zero is a valid pin number and would
    // claim GPIO0, which is this board's boot button.
    bus_config.data4_io_num = GPIO_NUM_NC;
    bus_config.data5_io_num = GPIO_NUM_NC;
    bus_config.data6_io_num = GPIO_NUM_NC;
    bus_config.data7_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = static_cast<int>(kLcdTransferBytes);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kLcdSpiHost, &bus_config, SPI_DMA_CH_AUTO), kTag,
                        "initialize QSPI bus failed");

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = board::kLcdCs;
    // A QSPI panel carries no command/data line; the command framing below
    // tells the driver where the command ends.
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = board::kLcdSpiMode;
    // The component configures QSPI at 20 MHz; this panel is specified at 40.
    io_config.pclk_hz = board::kLcdPixelClockHz;
    io_config.trans_queue_depth = 2;
    io_config.lcd_cmd_bits = board::kLcdCommandBits;
    io_config.lcd_param_bits = board::kLcdParameterBits;
    io_config.flags.quad_mode = true;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kLcdSpiHost), &io_config, &panel_io_), kTag,
        "create SPD2010 panel IO failed");

    spd2010_vendor_config_t vendor_config{};
    vendor_config.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(panel_io_, &panel_config, &panel_), kTag,
                        "create SPD2010 panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), kTag, "reset SPD2010 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_), kTag, "initialize SPD2010 failed");
    ESP_LOGI(kTag, "SPD2010 ready: %d x %d QSPI at %lu MHz, %u byte bands", board::kDisplayWidth, board::kDisplayHeight,
             static_cast<unsigned long>(board::kLcdPixelClockHz / 1000000U), static_cast<unsigned>(kLcdTransferBytes));
    return ESP_OK;
}

esp_err_t DisplayHardware::SetBrightness(int percent) {
    if (!backlight_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t bounded = static_cast<uint32_t>(std::clamp(percent, 0, 100));
    const uint32_t duty = (kBacklightMaximumDuty * bounded + 50U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel, duty), kTag, "set backlight duty failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel);
}

}  // namespace micropixel::platform::sensecap_watcher
