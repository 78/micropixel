// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "driver/i2c_master.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_ldo_regulator.h"
#include "host/ui/lvgl/square_common/profiles/portrait_800x1280.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/boards/reterminal-d1001/board_config.hpp"
#include "platform/boards/reterminal-d1001/display/dpi_framebuffers.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/usb_serial_jtag_local_control.hpp"

namespace micropixel::platform::reterminal_d1001 {

namespace ui_profile = host_ui::lvgl::square_common::profiles::portrait_800x1280;

inline constexpr int32_t kWidth = board::kDisplayWidth;
inline constexpr int32_t kHeight = board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);

// Board-owned input controls stay in internal RAM (the touch executor and the
// input router are reached from task and timer context).
struct D1001InputState final {
    buses::I2cExecutor system_i2c_executor{};
    buses::I2cExecutor touch_i2c_executor{};
    // The vendor driver supports up to three fingers; the ABI publishes the
    // same fixed capacity.
    input::EspLcdTouchInput touch_input{kWidth, kHeight, board::kTouchMaxPoints};
};

// Task-only board state lives in PSRAM; it references the controls above.
struct D1001State final {
    explicit D1001State(D1001InputState& input)
        : system_i2c_executor(input.system_i2c_executor),
          touch_i2c_executor(input.touch_i2c_executor),
          touch_input(input.touch_input) {}

    esp_ldo_channel_handle_t dsi_ldo{};
    esp_lcd_dsi_bus_handle_t dsi_bus{};
    esp_lcd_panel_io_handle_t panel_io{};
    esp_lcd_panel_handle_t panel{};
    lv_display_t* display{};
    // Touch controller on I2C0, separate from the system bus.
    i2c_master_bus_handle_t touch_i2c_bus{};
    esp_lcd_panel_io_handle_t touch_io{};
    esp_lcd_touch_handle_t touch{};
    // The panel's two DPI framebuffers, lent to the graphics engine so Guest
    // frames can bypass LVGL instead of being composited every frame.
    D1001DpiFramebuffers framebuffers{};
    buses::I2cExecutor& system_i2c_executor;
    buses::I2cExecutor& touch_i2c_executor;
    input::EspLcdTouchInput& touch_input;
    lvgl::FontRegistry fonts{};
    // The panel and the Host UI run RGB565 on this board.
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts, graphics::SurfacePixelFormat::kRgb565};
    host_ui::lvgl::square_common::StaticStatusLayerTransition status_transition{};
    // USB product channel: MPX1 install/control transport plus the development
    // display bridge used for screenshots and touch injection.
    transports::UsbSerialJtagLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, status_transition,
                                                         ui_profile::kSystemUiProfile};
};

}  // namespace micropixel::platform::reterminal_d1001