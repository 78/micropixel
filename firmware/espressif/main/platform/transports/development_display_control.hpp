#pragma once

#include <cstdint>
#include <expected>

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/transports/development_local_control.hpp"

namespace micropixel::platform::transports {

// Board-provided capture used when the LVGL draw buffer is not what the panel
// shows (for example while a Direct Surface scans out to a panel framebuffer).
// Returning an error falls through to the generic LVGL capture.
struct DevelopmentCaptureHook final {
    using Capture = std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> (*)(void* context);
    Capture capture{};
    void* context{};
};

// Development-only display commands shared by LVGL boards. This bridge owns
// the local-control protocol; frame capture and JPEG encoding remain in LVGL.
class DevelopmentDisplayControl final {
   public:
    [[nodiscard]] esp_err_t Start(lv_display_t* display, device::Input& input,
                                  DevelopmentLocalControlTransport& transport, uint32_t width, uint32_t height,
                                  lvgl::DisplayCaptureSource display_source = {},
                                  DevelopmentCaptureHook capture_hook = {});

   private:
    static void ReceiveCommand(void* context, const char* command);
    void ProcessCommand(const char* command);
    void CaptureAndTransmit();

    lv_display_t* display_{};
    device::Input* input_{};
    DevelopmentLocalControlTransport* transport_{};
    uint32_t sequence_{};
    uint32_t width_{};
    uint32_t height_{};
    lvgl::DisplayCaptureSource display_source_{};
    DevelopmentCaptureHook capture_hook_{};
};

}  // namespace micropixel::platform::transports
