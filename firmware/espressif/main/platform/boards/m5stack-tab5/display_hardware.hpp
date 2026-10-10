// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "host/ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::platform::m5stack_tab5 {

struct Tab5State;

// MIPI-DSI PHY power, panel transport and the ILI9881C panel itself.
// Call after the board hardware raised the panel/touch reset lines.
[[nodiscard]] esp_err_t InitializeDisplayHardware(Tab5State& state);

// Registers the panel with the shared LVGL adapter: 720x1280 native scanout
// rotated 90 degrees to a 1280x720 landscape canvas.
[[nodiscard]] esp_err_t RegisterLvglDisplay(Tab5State& state);

// Screenshot source for USB and Remote Control. Reads the frame buffer the
// panel is scanning out: while a Guest owns the panel the presenter flips
// between the two, so the displayed one is resolved at capture time.
[[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureDisplayedFrame(
    lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t width, uint32_t height);

}  // namespace micropixel::platform::m5stack_tab5
