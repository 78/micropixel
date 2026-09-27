// SPDX-FileCopyrightText: 2026 wamr-host contributors
// SPDX-License-Identifier: Apache-2.0

#include "platform/boards/sensecap-watcher/touch_hardware.hpp"

#include "esp_check.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform/boards/sensecap-watcher/board_config.hpp"

namespace micropixel::platform::sensecap_watcher {
namespace {

constexpr char kTag[] = "sensecap_watcher_touch";

// The SPD2010 is one die shared by the display and the touch controller, so the
// panel bring-up resets both. The touch controller comes back in its bootrom:
// the vendored driver sees the STATUS_TIC_IN_BIOS bit on its first report
// request and starts the controller's CPU, which is the vendor protocol, but
// the controller then answers no I2C at all until its own firmware runs. Every
// attempt in that window fails inside the driver and inside the IDF panel IO,
// and both log at error level - three alarming lines per attempt for what only
// means "the part is not up yet". Wait the window out here, before the Host UI
// starts polling, so the log reads as bring-up instead of as a fault. These two
// tags are used by nothing else on this board; if the controller never answers,
// their levels are restored and the driver's own errors surface again, which is
// what a real fault should look like.
constexpr char kControllerLogTag[] = "SPD2010";
constexpr char kPanelIoLogTag[] = "lcd_panel.io.i2c";
constexpr uint32_t kControllerWaitMs = 400U;
constexpr uint32_t kControllerPollMs = 20U;

// Best effort by design: a controller that is slow to answer must not fail the
// board bring-up, because the input layer keeps polling either way.
void WaitForControllerFirmware(esp_lcd_touch_handle_t touch) {
    const esp_log_level_t controller_level = esp_log_level_get(kControllerLogTag);
    const esp_log_level_t panel_io_level = esp_log_level_get(kPanelIoLogTag);
    esp_log_level_set(kControllerLogTag, ESP_LOG_NONE);
    esp_log_level_set(kPanelIoLogTag, ESP_LOG_NONE);

    const int64_t started_us = esp_timer_get_time();
    uint32_t elapsed_ms = 0U;
    esp_err_t status = ESP_ERR_TIMEOUT;
    do {
        // The first call is part of the bring-up rather than a probe: it is the
        // successful status read that hands the controller its start command.
        status = esp_lcd_touch_read_data(touch);
        elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000);
        if (status == ESP_OK || status == ESP_ERR_INVALID_RESPONSE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(kControllerPollMs));
    } while (elapsed_ms < kControllerWaitMs);

    esp_log_level_set(kControllerLogTag, controller_level);
    esp_log_level_set(kPanelIoLogTag, panel_io_level);

    if (status == ESP_OK || status == ESP_ERR_INVALID_RESPONSE) {
        ESP_LOGI(kTag, "SPD2010 controller left its bootrom after %u ms", static_cast<unsigned>(elapsed_ms));
    } else {
        ESP_LOGW(kTag, "SPD2010 controller silent for %u ms (%s); driver logs restored",
                 static_cast<unsigned>(kControllerWaitMs), esp_err_to_name(status));
    }
}

}  // namespace

esp_err_t TouchHardware::Initialize() {
    if (bus_ != nullptr || io_ != nullptr || touch_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // The panel bring-up drove this bus's two lines low once, as the vendor
    // does; creating the master bus hands them to the peripheral.
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = static_cast<i2c_port_num_t>(board::kTouchI2cPort);
    bus_config.sda_io_num = board::kTouchSda;
    bus_config.scl_io_num = board::kTouchScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.intr_priority = 0;
    bus_config.trans_queue_depth = 0;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus_), kTag, "create touch I2C bus failed");

    // The component's I2C config macro is C-only, like the panel ones, so the
    // fields it sets are set explicitly.
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS;
    io_config.scl_speed_hz = board::kTouchI2cClockHz;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 0;
    io_config.flags.disable_control_phase = 1;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus_, &io_config, &io_), kTag, "create touch panel IO failed");

    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = static_cast<uint16_t>(board::kDisplayWidth);
    touch_config.y_max = static_cast<uint16_t>(board::kDisplayHeight);
    // The vendor leaves the touch reset unconnected and shares nothing with the
    // panel, which has no reset line either.
    touch_config.rst_gpio_num = GPIO_NUM_NC;
    // No GPIO interrupt: the line is on the expander. A negative pin makes the
    // shared touch layer report "not supported", which is what selects polling
    // in the input adapter.
    touch_config.int_gpio_num = GPIO_NUM_NC;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    touch_config.flags.swap_xy = 0;
    touch_config.flags.mirror_x = 0;
    touch_config.flags.mirror_y = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_spd2010(io_, &touch_config, &touch_), kTag,
                        "initialize SPD2010 touch failed");
    WaitForControllerFirmware(touch_);
    ESP_LOGI(kTag, "SPD2010 touch ready: I2C%d at 0x%02X, %u Hz", board::kTouchI2cPort,
             static_cast<unsigned>(ESP_LCD_TOUCH_IO_I2C_SPD2010_ADDRESS),
             static_cast<unsigned>(board::kTouchI2cClockHz));
    return ESP_OK;
}

}  // namespace micropixel::platform::sensecap_watcher
