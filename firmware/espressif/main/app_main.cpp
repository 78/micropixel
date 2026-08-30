#include "firmware_app.hpp"
#include "host/logging/system_log_buffer.hpp"
#include "platform/platform.hpp"

extern "C" void app_main(void) {
    (void)micropixel::firmware::logging::StartSystemLogCapture();
    micropixel::firmware::FirmwareApp app(micropixel::platform::ConfiguredPlatform());
    app.Run();
}
