#pragma once

#include <cstdint>
#include <expected>

#include "device/local_control.hpp"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "host_ui/system_ui.hpp"
#include "platform/drivers/input/gt911_input.hpp"

struct _lv_display_t;
using lv_display_t = _lv_display_t;  // NOLINT(readability-identifier-naming)

namespace micropixel::platform::metalio_claw4 {

// Host USB development transport; present in the P4 product firmware and kept
// outside the Guest ABI.
[[nodiscard]] esp_err_t InitializeScreenCapture(lv_display_t* display, drivers::Gt911Input& touch_input, uint32_t width,
                                                uint32_t height);
[[nodiscard]] device::LocalControlBackend& UsbLocalControlBackend();

// Synchronous Host-task capture used by Remote Control. The displayed panel
// framebuffer is encoded by the ESP32-P4 JPEG peripheral into PSRAM.
[[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg(
    lv_display_t* display, esp_lcd_panel_handle_t panel, uint32_t width, uint32_t height);

}  // namespace micropixel::platform::metalio_claw4
