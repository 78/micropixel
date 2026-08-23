#ifndef MICROPIXEL_FIRMWARE_APP_HPP
#define MICROPIXEL_FIRMWARE_APP_HPP

#include <expected>

namespace micropixel::platform {
class Platform;
}

namespace micropixel::firmware {

// Composition root for platform initialization and one Guest runtime session.
// app_main() deliberately owns only this object so ESP-IDF entry-point details
// do not leak into Runtime or device services.
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
        kPthreadConfiguration,
        kPthreadAttributes,
        kThreadCreation,
        kThreadJoin,
    };

    [[nodiscard]] std::expected<void, StartupError> InitializePlatform();
    [[nodiscard]] std::expected<void, StartupError> RunGuest();
    [[noreturn]] static void RestartAfterGuest();

    platform::Platform& platform_;
};

}  // namespace micropixel::firmware

#endif
