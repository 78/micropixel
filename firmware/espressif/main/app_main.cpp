#include "firmware_app.hpp"
#include "host/logging/system_log_buffer.hpp"
#include "platform/diagnostics/startup_timing.hpp"
#include "platform/platform.hpp"

extern "C" void app_main(void) {
    micropixel::platform::diagnostics::BeginStartupTiming();
    (void)micropixel::firmware::logging::StartSystemLogCapture();
    micropixel::platform::diagnostics::MarkStartupTiming("log_capture_ready");
    micropixel::firmware::FirmwareApp app(micropixel::platform::ConfiguredPlatform());
    micropixel::platform::diagnostics::MarkStartupTiming("platform_constructed");
    app.Run();
}
