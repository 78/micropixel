#ifndef MICROPIXEL_RUNTIME_APP_SESSION_HPP
#define MICROPIXEL_RUNTIME_APP_SESSION_HPP

#include <array>
#include <atomic>
#include <expected>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "runtime/bundle/aot_package.hpp"
#include "runtime/wamr/wamr_runtime.hpp"

namespace micropixel::device {
class DeviceServices;
}

namespace micropixel::runtime {

class GuestContext;
class DecodedBitmap;
class GuestLogSink;

enum class AppSessionError {
    kPackageLoad,
    kLaunchBitmap,
    kModuleLoad,
    kGuestInstantiation,
    kExecEnvironment,
    kMissingEntry,
    kGuestServices,
    kConformanceTestHook,
    kWatchdogTimeout,
    kGuestTrap,
    kGuestExit,
    kSessionAlreadyActive,
    kRuntimeSynchronization,
};

struct AppSessionFailure final {
    AppSessionError code{};
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id{};
};

// Owns every resource belonging to one runnable Guest. Destruction tears the
// instance down in reverse order and leaves process-wide WAMR state intact.
class AppSession final {
   public:
    AppSession(const AppSession&) = delete;
    AppSession& operator=(const AppSession&) = delete;
    AppSession(AppSession&& other) noexcept;
    AppSession& operator=(AppSession&& other) = delete;
    ~AppSession();

    [[nodiscard]] static std::expected<AppSession, AppSessionFailure> Create(device::DeviceServices& devices,
                                                                             const bundlefs_file_t& file,
                                                                             GuestLogSink* log_sink = nullptr);
    [[nodiscard]] std::expected<void, AppSessionError> Run();
    [[nodiscard]] bool Suspend(TickType_t timeout);
    [[nodiscard]] bool Resume();
    void RequestStop();
    void ForceStop();
    [[nodiscard]] const char* app_id() const;  // NOLINT(readability-identifier-naming)

   private:
    AppSession(device::DeviceServices& devices, AotPackage package, LoadedModule module, GuestInstance guest,
               std::unique_ptr<GuestContext> context, std::unique_ptr<GuestContextBinding> context_binding,
               std::unique_ptr<DecodedBitmap> launch_bitmap, bool launch_visible);

    device::DeviceServices& devices_;
    AotPackage package_;
    LoadedModule module_;
    GuestInstance guest_;
    std::unique_ptr<GuestContext> context_;
    std::unique_ptr<GuestContextBinding> context_binding_;
    std::unique_ptr<DecodedBitmap> launch_bitmap_;
    std::atomic<bool> stop_requested_{};
    bool launch_visible_{};
};

}  // namespace micropixel::runtime

#endif
