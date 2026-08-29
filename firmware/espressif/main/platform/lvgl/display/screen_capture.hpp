#pragma once

#include <cstdint>
#include <expected>

#include "host/ui/system_ui.hpp"

struct _lv_display_t;
using lv_display_t = _lv_display_t;  // NOLINT(readability-identifier-naming)

namespace micropixel::platform::lvgl {

enum class DisplayCapturePixelFormat : uint8_t {
    kRgb565,
    kRgb888,
};

// Optional stable, canonical-color shadow of the pixels most recently sent to
// the panel. Partial-buffer displays provide this because their active LVGL
// draw buffer never contains a complete frame.
struct DisplayCaptureSource final {
    const uint8_t* pixels{};
    uint32_t stride{};
    DisplayCapturePixelFormat format{DisplayCapturePixelFormat::kRgb565};
    const bool* ready{};
};

// Captures a stable full-frame display source and encodes it with the SoC JPEG
// peripheral. The returned buffer owns its bytes and may be detached into a
// Host control artifact.
[[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg(
    lv_display_t* display, uint32_t width, uint32_t height, DisplayCaptureSource display_source = {});

}  // namespace micropixel::platform::lvgl
