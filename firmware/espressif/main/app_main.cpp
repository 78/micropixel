#include "firmware_app.hpp"
#include "platform/platform.hpp"

extern "C" void app_main(void) {
    micropixel::firmware::FirmwareApp app(micropixel::platform::ConfiguredPlatform());
    app.Run();
}
