#ifndef MICROPIXEL_FIRMWARE_APP_HPP
#define MICROPIXEL_FIRMWARE_APP_HPP

#include <expected>

#include "runtime/app_runtime.hpp"

namespace micropixel::platform {
class Platform;
}

namespace micropixel::firmware {

// Composition root for the long-lived Host, its reusable AppRuntime, and the
// App Hall driven Guest sessions.
class FirmwareApp final {
   public:
    explicit FirmwareApp(platform::Platform& platform) : platform_(platform) {}
    FirmwareApp(const FirmwareApp&) = delete;
    FirmwareApp& operator=(const FirmwareApp&) = delete;

    void Run();

   private:
    enum class StartupError {
        kNvsInitialization,
        kPlatformInitialization,
    };

    [[nodiscard]] std::expected<void, StartupError> InitializePlatform();

    platform::Platform& platform_;
};

}  // namespace micropixel::firmware

#endif
