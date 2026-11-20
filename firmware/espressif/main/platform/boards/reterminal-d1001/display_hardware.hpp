// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "host/ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::platform::reterminal_d1001 {

struct D1001State;

// Bring up the MIPI-DSI JD9365 panel: DSI PHY LDO, two-lane bus, DBI control
// channel, DPI scanout with two framebuffers and the vendor init table.
[[nodiscard]] esp_err_t InitializeDisplayHardware(D1001State& state);
// Register the panel with esp_lv_adapter in DOUBLE_DIRECT mode and lend the DPI
// framebuffers to the Guest graphics engine.
[[nodiscard]] esp_err_t RegisterLvglDisplay(D1001State& state);
// Read the framebuffer currently being scanned out and encode it as JPEG.
[[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureDisplayedFrame(
    lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t width, uint32_t height);

}  // namespace micropixel::platform::reterminal_d1001