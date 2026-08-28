#pragma once

#include <cstdint>

#include "device/input.hpp"
#include "esp_err.h"
#include "platform/transports/development_local_control.hpp"

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

// Development-only screen capture shared by LVGL boards. Captures are encoded
// by the SoC JPEG peripheral and streamed over the board's USB local-control
// transport; this is not part of the Guest ABI.
class ScreenCaptureDevelopment final {
   public:
    enum class Source : uint8_t {
        kLogical,
        kDisplayBuffer,
    };

    [[nodiscard]] esp_err_t Start(lv_display_t* display, device::InputBackend& input,
                                  transports::DevelopmentLocalControlTransport& transport, uint32_t width,
                                  uint32_t height, DisplayCaptureSource display_source = {});

   private:
    static void ReceiveDevelopmentCommand(void* context, const char* command);
    void ProcessCommand(const char* command);
    void CaptureAndTransmit(Source source);

    lv_display_t* display_{};
    device::InputBackend* input_{};
    transports::DevelopmentLocalControlTransport* transport_{};
    uint32_t sequence_{};
    uint32_t width_{};
    uint32_t height_{};
    DisplayCaptureSource display_source_{};
};

}  // namespace micropixel::platform::lvgl
