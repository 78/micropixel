#include "runtime/app_runtime.hpp"

#include <cstdio>
#include <utility>

#include "device/device_services.hpp"
#include "esp_log.h"
#include "runtime/abi/abi_bridge.h"
#include "runtime/guest_log_sink.hpp"
#include "runtime/wamr/diagnostics.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_runtime";

void CopyAppId(const char* source, std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U>& destination) {
    (void)std::snprintf(destination.data(), destination.size(), "%s", source);
}

}  // namespace

AppRuntime::AppRuntime(device::DeviceServices& devices, WamrRuntime wamr, SemaphoreHandle_t session_mutex,
                       GuestLogSink* log_sink)
    : devices_(devices), log_sink_(log_sink), wamr_(std::move(wamr)), session_mutex_(session_mutex) {}

AppRuntime::AppRuntime(AppRuntime&& other) noexcept
    : devices_(other.devices_),
      log_sink_(other.log_sink_),
      wamr_(std::move(other.wamr_)),
      session_mutex_(std::exchange(other.session_mutex_, nullptr)),
      active_session_(std::exchange(other.active_session_, nullptr)),
      session_active_(std::exchange(other.session_active_, false)),
      stop_requested_(std::exchange(other.stop_requested_, false)) {}

AppRuntime::~AppRuntime() {
    if (session_mutex_ != nullptr) {
        vSemaphoreDelete(session_mutex_);
        session_mutex_ = nullptr;
    }
}

bool AppRuntime::TakeSessionLock() {
    return session_mutex_ != nullptr && xSemaphoreTake(session_mutex_, portMAX_DELAY) == pdTRUE;
}

void AppRuntime::GiveSessionLock() {
    if (session_mutex_ != nullptr) {
        (void)xSemaphoreGive(session_mutex_);
    }
}

std::expected<AppRuntime, AppRuntimeError> AppRuntime::Initialize(device::DeviceServices& devices,
                                                                  GuestLogSink* log_sink) {
    auto runtime_result = WamrRuntime::Initialize();
    if (!runtime_result) {
        ESP_LOGE(kTag, "%s", runtime_result.error().message.data());
        return std::unexpected(AppRuntimeError::kRuntimeInitialization);
    }
    WamrRuntime wamr = std::move(*runtime_result);
    micropixel_log_heap_state("WAMR initialized");
    ESP_LOGI(kTag, "Guest quotas: max child threads=%d, watchdog=%d ms", CONFIG_WAMR_RUNTIME_MAX_GUEST_THREADS,
             CONFIG_WAMR_DEFAULT_WATCHDOG_TIMEOUT_MS);

    if (!micropixel_register_native_apis()) {
        ESP_LOGE(kTag, "failed to register Host native APIs");
        return std::unexpected(AppRuntimeError::kNativeApiRegistration);
    }
    SemaphoreHandle_t session_mutex = xSemaphoreCreateMutex();
    if (session_mutex == nullptr) {
        ESP_LOGE(kTag, "failed to create AppSession synchronization mutex");
        return std::unexpected(AppRuntimeError::kSynchronization);
    }
    ESP_LOGI(kTag, "process-wide WAMR runtime ready");
    return AppRuntime(devices, std::move(wamr), session_mutex, log_sink);
}

AppRunOutcome AppRuntime::RunApp(const InstalledApp& app, AppSessionReadySink ready_sink, void* ready_context) {
    AppRunOutcome outcome;
    CopyAppId(app.app_id.data(), outcome.app_id);
    if (!TakeSessionLock()) {
        outcome.error = AppSessionError::kRuntimeSynchronization;
        ESP_LOGE(kTag, "unable to lock AppSession state");
        return outcome;
    }
    if (session_active_) {
        GiveSessionLock();
        outcome.error = AppSessionError::kSessionAlreadyActive;
        ESP_LOGE(kTag, "rejected a second concurrent AppSession");
        return outcome;
    }
    session_active_ = true;
    stop_requested_ = false;
    GiveSessionLock();

    micropixel_log_heap_state("AppSession create begin");
    auto session_result = AppSession::Create(devices_, app.file, log_sink_);
    if (!session_result) {
        if (session_result.error().app_id[0] != '\0') {
            outcome.app_id = session_result.error().app_id;
        }
        outcome.error = session_result.error().code;
        if (TakeSessionLock()) {
            if (stop_requested_) {
                outcome.completion = AppCompletion::kStopped;
            }
            session_active_ = false;
            stop_requested_ = false;
            GiveSessionLock();
        }
        micropixel_log_heap_state("AppSession create failed");
        return outcome;
    }

    {
        AppSession session = std::move(*session_result);
        CopyAppId(session.app_id(), outcome.app_id);
        bool stop_before_run = false;
        if (TakeSessionLock()) {
            active_session_ = &session;
            stop_before_run = stop_requested_;
            GiveSessionLock();
        }
        if (ready_sink != nullptr) {
            ready_sink(ready_context);
        }
        if (stop_before_run) {
            session.RequestStop();
        }
        auto run_result = session.Run();
        bool stopped = false;
        if (TakeSessionLock()) {
            stopped = stop_requested_;
            active_session_ = nullptr;
            GiveSessionLock();
        }
        if (stopped) {
            outcome.completion = AppCompletion::kStopped;
        } else if (run_result) {
            outcome.completion = AppCompletion::kExited;
        } else {
            outcome.error = run_result.error();
        }
    }

    if (TakeSessionLock()) {
        session_active_ = false;
        stop_requested_ = false;
        GiveSessionLock();
    }

    micropixel_log_heap_state("AppSession cleanup complete");
    micropixel_log_stack_profiles();
    const char* completion = outcome.completion == AppCompletion::kExited
                                 ? "exited"
                                 : (outcome.completion == AppCompletion::kStopped ? "stopped" : "failed");
    ESP_LOGI(kTag, "AppSession finished: app=%s result=%s", outcome.app_id.data(), completion);
    return outcome;
}

bool AppRuntime::RequestStop() {
    if (!TakeSessionLock()) {
        return false;
    }
    if (!session_active_) {
        GiveSessionLock();
        return false;
    }
    stop_requested_ = true;
    if (active_session_ != nullptr) {
        active_session_->RequestStop();
    }
    GiveSessionLock();
    return true;
}

bool AppRuntime::ForceStop() {
    if (!TakeSessionLock()) {
        return false;
    }
    if (!session_active_ || active_session_ == nullptr) {
        GiveSessionLock();
        return false;
    }
    stop_requested_ = true;
    active_session_->ForceStop();
    GiveSessionLock();
    return true;
}

bool AppRuntime::RequestSuspend(TickType_t timeout) {
    if (!TakeSessionLock()) {
        return false;
    }
    const bool suspended = active_session_ != nullptr && active_session_->Suspend(timeout);
    GiveSessionLock();
    return suspended;
}

bool AppRuntime::RequestResume() {
    if (!TakeSessionLock()) {
        return false;
    }
    const bool resumed = active_session_ != nullptr && active_session_->Resume();
    GiveSessionLock();
    return resumed;
}

}  // namespace micropixel::runtime
