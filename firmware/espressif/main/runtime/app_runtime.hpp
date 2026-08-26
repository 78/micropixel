#ifndef MICROPIXEL_RUNTIME_APP_RUNTIME_HPP
#define MICROPIXEL_RUNTIME_APP_RUNTIME_HPP

#include <array>
#include <expected>
#include <string_view>

#include "abi/micropixel_abi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "runtime/app_session.hpp"
#include "runtime/wamr/wamr_runtime.hpp"

namespace micropixel::device {
class DeviceServices;
}

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::runtime {

class GuestLogSink;

enum class AppRuntimeError {
    kRuntimeInitialization,
    kNativeApiRegistration,
    kSynchronization,
};

enum class AppCompletion {
    kExited,
    kStopped,
    kFailed,
};

struct AppRunOutcome final {
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id{};
    AppCompletion completion{AppCompletion::kFailed};
    AppSessionError error{AppSessionError::kPackageLoad};
    std::array<char, 256U> detail{};
    int32_t exit_code{};
    bool has_exit_code{};
};

using AppSessionReadySink = void (*)(void* context);

// Owns process-wide WAMR state. RunApp creates at most one
// synchronous AppSession while this object remains alive and reusable.
class AppRuntime final {
   public:
    AppRuntime(const AppRuntime&) = delete;
    AppRuntime& operator=(const AppRuntime&) = delete;
    AppRuntime(AppRuntime&& other) noexcept;
    AppRuntime& operator=(AppRuntime&& other) = delete;
    ~AppRuntime();

    [[nodiscard]] static std::expected<AppRuntime, AppRuntimeError> Initialize(
        device::DeviceServices& devices, work::BackgroundExecutor& background_executor,
        std::string_view effective_locale, GuestLogSink* log_sink = nullptr);
    [[nodiscard]] bool SetEffectiveLocale(std::string_view effective_locale);
    [[nodiscard]] AppRunOutcome RunApp(const InstalledApp& app, AppSessionReadySink ready_sink = nullptr,
                                       void* ready_context = nullptr);
    [[nodiscard]] bool RequestSuspend(TickType_t timeout);
    [[nodiscard]] bool RequestResume();
    [[nodiscard]] bool RequestStop();
    [[nodiscard]] bool ForceStop();

   private:
    AppRuntime(device::DeviceServices& devices, work::BackgroundExecutor& background_executor, WamrRuntime wamr,
               SemaphoreHandle_t session_mutex, std::string_view effective_locale, GuestLogSink* log_sink);
    [[nodiscard]] bool TakeSessionLock();
    void GiveSessionLock();

    device::DeviceServices& devices_;
    work::BackgroundExecutor& background_executor_;
    GuestLogSink* log_sink_{};
    WamrRuntime wamr_;
    SemaphoreHandle_t session_mutex_{};
    AppSession* active_session_{};
    std::array<char, MICROPIXEL_LOCALE_TAG_MAX_BYTES + 1U> effective_locale_{};
    bool session_active_{};
    bool stop_requested_{};
};

}  // namespace micropixel::runtime

#endif
