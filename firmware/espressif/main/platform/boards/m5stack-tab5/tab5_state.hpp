// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "host/ui/lvgl/square_common/profiles/portrait_720x1280.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/boards/m5stack-tab5/board_config.hpp"
#include "platform/boards/m5stack-tab5/display/dpi_framebuffers.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/gt911_input.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/usb_serial_jtag_local_control.hpp"

namespace micropixel::platform::m5stack_tab5 {

namespace ui_profile = host_ui::lvgl::square_common::profiles::portrait_720x1280;

inline constexpr int32_t kWidth = board::kDisplayWidth;
inline constexpr int32_t kHeight = board::kDisplayHeight;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);

// Board-owned input controls stay in internal RAM (ISR and I2C worker access).
struct Tab5InputState final {
    buses::I2cExecutor i2c_executor{};
    // The panel scans out in the orientation the controller reports, so the
    // GT911 coordinates are already the logical ones the Host UI and the Guest
    // ABI use: there is no board-side coordinate mapping to apply.
    input::Gt911Input touch_input{kWidth, kHeight, board::kTouchInterrupt};
};

// Task-only board state lives in PSRAM; it references the controls above.
struct Tab5State final {
    explicit Tab5State(Tab5InputState& input) : i2c_executor(input.i2c_executor), touch_input(input.touch_input) {}

    esp_ldo_channel_handle_t dsi_ldo{};
    esp_lcd_dsi_bus_handle_t dsi_bus{};
    esp_lcd_panel_io_handle_t panel_io{};
    esp_lcd_panel_handle_t panel{};
    lv_display_t* display{};
    // The panel's two DPI framebuffers, lent to the graphics engine so Guest
    // frames can bypass LVGL instead of being composited every frame.
    Tab5DpiFramebuffers framebuffers{};
    buses::I2cExecutor& i2c_executor;
    input::Gt911Input& touch_input;
    lvgl::FontRegistry fonts{};
    // Panel, DSI transport and Host UI run RGB565 on this board, matching the
    // vendor demo. Hardware validation still has to confirm the P4 rev v1.x
    // DSI scanout of this format.
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts, graphics::SurfacePixelFormat::kRgb565};
    host_ui::lvgl::square_common::StaticStatusLayerTransition status_transition{};
    // USB product channel: MPX1 install/control transport plus the development
    // display bridge used for screenshots and touch injection.
    transports::UsbSerialJtagLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, status_transition,
                                                         ui_profile::kSystemUiProfile};
};

}  // namespace micropixel::platform::m5stack_tab5
