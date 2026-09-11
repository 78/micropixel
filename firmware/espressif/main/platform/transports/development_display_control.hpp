#pragma once

#include <cstdint>
#include <expected>

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "host/ui/lvgl/square_common/square_presentation.hpp"
#include "platform/transports/development_local_control.hpp"

namespace micropixel::platform::transports {

// Board-provided screenshot: the same capture the board's presentation
// gives Remote Control, so USB and remote screenshots can never diverge.
struct DevelopmentCaptureHook final {
    using Capture = std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> (*)(void* context);
    Capture capture{};
    void* context{};

    // Forwards to a presentation's screenshot; `screen` must outlive the hook.
    [[nodiscard]] static DevelopmentCaptureHook For(host_ui::lvgl::square_common::ScreenCapture& screen);
};

// Development-only display commands shared by LVGL boards. This bridge owns
// the local-control protocol; frame capture and JPEG encoding stay with the
// board's presentation.
class DevelopmentDisplayControl final {
   public:
    [[nodiscard]] esp_err_t Start(device::Input& input, DevelopmentLocalControlTransport& transport, uint32_t width,
                                  uint32_t height, DevelopmentCaptureHook capture_hook);

   private:
    static void ReceiveCommand(void* context, const char* command);
    void ProcessCommand(const char* command);
    void CaptureAndTransmit();

    device::Input* input_{};
    DevelopmentLocalControlTransport* transport_{};
    uint32_t sequence_{};
    // Panel size in pixels; bounds injected touches.
    uint32_t width_{};
    uint32_t height_{};
    DevelopmentCaptureHook capture_hook_{};
};

}  // namespace micropixel::platform::transports
