#ifndef MICROPIXEL_FIRMWARE_APP_CONTROLLER_HPP
#define MICROPIXEL_FIRMWARE_APP_CONTROLLER_HPP

#include <array>
#include <atomic>
#include <expected>
#include <optional>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "pthread.h"
#include "runtime/app_runtime.hpp"

namespace micropixel::firmware {

enum class AppLifecycleState {
    kNotRunning,
    kStarting,
    kForeground,
    kSuspending,
    kSuspended,
    kResuming,
    kStopping,
};

enum class AppControllerError {
    kUnavailable,
    kAppAlreadyActive,
    kPthreadConfiguration,
    kPthreadAttributes,
    kThreadCreation,
    kThreadJoin,
    kInvalidState,
    kSuspendTimeout,
    kResumeFailed,
    kTerminationTimeout,
};

// Long-lived Host-side owner for the one allowed Guest execution thread.
// Completion is delivered through a bounded queue so HostController can remain
// responsive to future System Shell actions instead of joining immediately.
class AppController final {
   public:
    explicit AppController(runtime::AppRuntime& runtime);
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;
    ~AppController();

    [[nodiscard]] bool valid() const { return completion_queue_ != nullptr; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] std::expected<void, AppControllerError> Start(const runtime::InstalledApp& app);
    [[nodiscard]] std::expected<void, AppControllerError> Suspend(TickType_t timeout);
    [[nodiscard]] std::expected<void, AppControllerError> Resume();
    [[nodiscard]] bool RequestStop();
    [[nodiscard]] bool ForceStop();
    [[nodiscard]] std::expected<runtime::AppRunOutcome, AppControllerError> Stop(TickType_t cooperative_timeout,
                                                                                 TickType_t forced_timeout);
    [[nodiscard]] std::expected<std::optional<runtime::AppRunOutcome>, AppControllerError> PollCompletion(
        TickType_t timeout);
    [[nodiscard]] AppLifecycleState state() const {  // NOLINT(readability-identifier-naming)
        return state_.load(std::memory_order_acquire);
    }

   private:
    static void* RunSession(void* context);
    static void SessionReady(void* context);

    runtime::AppRuntime& runtime_;
    runtime::InstalledApp selected_app_{};
    StaticQueue_t completion_queue_storage_{};
    std::array<uint8_t, sizeof(runtime::AppRunOutcome)> completion_queue_bytes_{};
    QueueHandle_t completion_queue_{};
    pthread_t thread_{};
    std::atomic<AppLifecycleState> state_{AppLifecycleState::kNotRunning};
    std::atomic<bool> stop_requested_{false};
    bool joinable_{};
};

}  // namespace micropixel::firmware

#endif
