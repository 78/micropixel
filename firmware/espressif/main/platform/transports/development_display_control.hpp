#pragma once

#include <cstdint>

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "platform/lvgl/display/screen_capture.hpp"
#include "platform/transports/development_local_control.hpp"

namespace micropixel::platform::transports {

// Development-only display commands shared by LVGL boards. This bridge owns
// the local-control protocol; frame capture and JPEG encoding remain in LVGL.
class DevelopmentDisplayControl final {
   public:
    [[nodiscard]] esp_err_t Start(lv_display_t* display, device::Input& input,
                                  DevelopmentLocalControlTransport& transport, uint32_t width, uint32_t height,
                                  lvgl::DisplayCaptureSource display_source = {});

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
};

}  // namespace micropixel::platform::transports
