#pragma once

#include <cstdint>
#include <expected>

#include "device/contracts/local_control.hpp"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "host/ui/system_ui.hpp"
#include "platform/input/gt911_input.hpp"
#include "platform/transports/development_display_control.hpp"

struct _lv_display_t;
using lv_display_t = _lv_display_t;  // NOLINT(readability-identifier-naming)

namespace micropixel::platform::metalio_claw4 {

// Host USB development transport; present in the P4 product firmware and kept
// outside the Guest ABI. `capture_hook` is the board presentation's
// screenshot, shared with Remote Control.
[[nodiscard]] esp_err_t InitializeScreenCapture(input::Gt911Input& touch_input, uint32_t width, uint32_t height,
                                                transports::DevelopmentCaptureHook capture_hook);
[[nodiscard]] device::LocalControl& UsbLocalControl();

// Synchronous Host-task screenshot. While a Direct Surface or system
// transition owns dummy draw the displayed DPI framebuffer is encoded by the
// ESP32-P4 JPEG peripheral into PSRAM; otherwise the LVGL draw buffer is the
// screen content and the generic locked capture is used.
[[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg(
    lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t width, uint32_t height);

}  // namespace micropixel::platform::metalio_claw4
