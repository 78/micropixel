#include "app_controller.hpp"

#include <cstddef>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "freertos/task.h"
#include "task_policy.hpp"

namespace micropixel::firmware {
namespace {

constexpr char kTag[] = "micropixel_apps";
constexpr size_t kWamrTaskStackSize = 16U * 1024U;
constexpr int kWamrTaskCore = 0;

class PthreadAttributes final {
   public:
    PthreadAttributes() : initialized_(pthread_attr_init(&attributes_) == 0) {}
    PthreadAttributes(const PthreadAttributes&) = delete;
    PthreadAttributes& operator=(const PthreadAttributes&) = delete;

    ~PthreadAttributes() {
        if (initialized_) {
            (void)pthread_attr_destroy(&attributes_);
        }
    }

    [[nodiscard]] bool valid() const { return initialized_; }  // NOLINT(readability-identifier-naming)

    [[nodiscard]] bool Configure(size_t stack_size) {
        return initialized_ && pthread_attr_setdetachstate(&attributes_, PTHREAD_CREATE_JOINABLE) == 0 &&
               pthread_attr_setstacksize(&attributes_, stack_size) == 0;
    }

    [[nodiscard]] const pthread_attr_t* native_handle() const {  // NOLINT(readability-identifier-naming)
        return &attributes_;
    }

   private:
    pthread_attr_t attributes_{};
    bool initialized_{};
};

class ScopedPthreadConfiguration final {
   public:
    ScopedPthreadConfiguration() {
        parent_config_found_ = esp_pthread_get_cfg(&parent_config_) == ESP_OK;
        esp_pthread_cfg_t wamr_config = parent_config_found_ ? parent_config_ : esp_pthread_get_default_config();
        wamr_config.pin_to_core = kWamrTaskCore;
        wamr_config.prio = static_cast<int>(task_policy::kGuestPriority);
        wamr_config.inherit_cfg = false;
        priority_ = wamr_config.prio;
        applied_ = esp_pthread_set_cfg(&wamr_config) == ESP_OK;
    }

    ScopedPthreadConfiguration(const ScopedPthreadConfiguration&) = delete;
    ScopedPthreadConfiguration& operator=(const ScopedPthreadConfiguration&) = delete;
    ~ScopedPthreadConfiguration() { Restore(); }

    [[nodiscard]] bool applied() const { return applied_; }   // NOLINT(readability-identifier-naming)
    [[nodiscard]] int priority() const { return priority_; }  // NOLINT(readability-identifier-naming)

    void Restore() {
        if (!applied_) {
            return;
        }
        if (parent_config_found_) {
            (void)esp_pthread_set_cfg(&parent_config_);
        } else {
            esp_pthread_cfg_t default_config = esp_pthread_get_default_config();
            (void)esp_pthread_set_cfg(&default_config);
        }
        applied_ = false;
    }

   private:
    esp_pthread_cfg_t parent_config_{};
    int priority_{};
    bool parent_config_found_{};
    bool applied_{};
};

}  // namespace

AppController::AppController(runtime::AppRuntime& runtime) : runtime_(runtime) {
    completion_queue_ = xQueueCreateStatic(1U, sizeof(runtime::AppRunOutcome), completion_queue_bytes_.data(),
                                           &completion_queue_storage_);
}

AppController::~AppController() {
    if (!joinable_) {
        return;
    }
    (void)RequestStop();
    (void)ForceStop();
    if (pthread_join(thread_, nullptr) != 0) {
        ESP_LOGE(kTag, "failed to join the Guest thread during AppController teardown");
    }
    joinable_ = false;
}

std::expected<void, AppControllerError> AppController::Start(const runtime::InstalledApp& app) {
    if (completion_queue_ == nullptr) {
        return std::unexpected(AppControllerError::kUnavailable);
    }
    if (joinable_ || state() != AppLifecycleState::kNotRunning) {
        return std::unexpected(AppControllerError::kAppAlreadyActive);
    }

    ScopedPthreadConfiguration configuration;
    if (!configuration.applied()) {
        return std::unexpected(AppControllerError::kPthreadConfiguration);
    }
    PthreadAttributes attributes;
    if (!attributes.valid() || !attributes.Configure(kWamrTaskStackSize)) {
        return std::unexpected(AppControllerError::kPthreadAttributes);
    }

    selected_app_ = app;
    stop_requested_.store(false, std::memory_order_release);
    state_.store(AppLifecycleState::kStarting, std::memory_order_release);
    (void)xQueueReset(completion_queue_);
    if (pthread_create(&thread_, attributes.native_handle(), RunSession, this) != 0) {
        state_.store(AppLifecycleState::kNotRunning, std::memory_order_release);
        return std::unexpected(AppControllerError::kThreadCreation);
    }
    joinable_ = true;
    ESP_LOGI(kTag, "WAMR task policy: core=%d priority=%d (FPU-stable affinity)", kWamrTaskCore,
             configuration.priority());
    configuration.Restore();
    return {};
}

bool AppController::RequestStop() {
    const AppLifecycleState current = state();
    if (!joinable_ || current == AppLifecycleState::kNotRunning) {
        return false;
    }
    stop_requested_.store(true, std::memory_order_release);
    state_.store(AppLifecycleState::kStopping, std::memory_order_release);
    (void)runtime_.RequestStop();
    return true;
}

bool AppController::ForceStop() { return joinable_ && state() == AppLifecycleState::kStopping && runtime_.ForceStop(); }

std::expected<runtime::AppRunOutcome, AppControllerError> AppController::Stop(TickType_t cooperative_timeout,
                                                                              TickType_t forced_timeout) {
    if (state() != AppLifecycleState::kStopping && !RequestStop()) {
        return std::unexpected(AppControllerError::kInvalidState);
    }

    constexpr TickType_t kPollTicks = pdMS_TO_TICKS(50);
    const TickType_t started = xTaskGetTickCount();
    bool force_announced = false;
    for (;;) {
        auto result = PollCompletion(kPollTicks);
        if (!result) {
            return std::unexpected(result.error());
        }
        if (result->has_value()) {
            return **result;
        }

        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed < cooperative_timeout) {
            continue;
        }
        if (!force_announced) {
            ESP_LOGW(kTag, "Guest did not accept cooperative Stop within %lu ticks; forcing termination",
                     static_cast<unsigned long>(cooperative_timeout));
            force_announced = true;
        }
        (void)ForceStop();
        if (elapsed - cooperative_timeout >= forced_timeout) {
            ESP_LOGE(kTag, "Guest thread did not terminate within the forced-stop deadline");
            return std::unexpected(AppControllerError::kTerminationTimeout);
        }
    }
}

std::expected<void, AppControllerError> AppController::Suspend(TickType_t timeout) {
    AppLifecycleState expected = AppLifecycleState::kForeground;
    if (!state_.compare_exchange_strong(expected, AppLifecycleState::kSuspending, std::memory_order_acq_rel)) {
        return std::unexpected(AppControllerError::kInvalidState);
    }
    if (!runtime_.RequestSuspend(timeout)) {
        stop_requested_.store(true, std::memory_order_release);
        state_.store(AppLifecycleState::kStopping, std::memory_order_release);
        (void)runtime_.RequestStop();
        return std::unexpected(AppControllerError::kSuspendTimeout);
    }
    state_.store(AppLifecycleState::kSuspended, std::memory_order_release);
    return {};
}

std::expected<void, AppControllerError> AppController::Resume() {
    AppLifecycleState expected = AppLifecycleState::kSuspended;
    if (!state_.compare_exchange_strong(expected, AppLifecycleState::kResuming, std::memory_order_acq_rel)) {
        return std::unexpected(AppControllerError::kInvalidState);
    }
    if (!runtime_.RequestResume()) {
        stop_requested_.store(true, std::memory_order_release);
        state_.store(AppLifecycleState::kStopping, std::memory_order_release);
        (void)runtime_.RequestStop();
        return std::unexpected(AppControllerError::kResumeFailed);
    }
    state_.store(AppLifecycleState::kForeground, std::memory_order_release);
    return {};
}

std::expected<std::optional<runtime::AppRunOutcome>, AppControllerError> AppController::PollCompletion(
    TickType_t timeout) {
    runtime::AppRunOutcome outcome;
    if (completion_queue_ == nullptr || xQueueReceive(completion_queue_, &outcome, timeout) != pdTRUE) {
        return std::optional<runtime::AppRunOutcome>{};
    }
    if (!joinable_ || pthread_join(thread_, nullptr) != 0) {
        if (joinable_) {
            (void)pthread_detach(thread_);
            joinable_ = false;
        }
        return std::unexpected(AppControllerError::kThreadJoin);
    }
    joinable_ = false;
    // A successful join proves RunSession has returned. Make this the final
    // lifecycle arbitration point as a concurrent failed suspend/resume
    // request may otherwise overwrite the worker's kNotRunning store with
    // kStopping.
    state_.store(AppLifecycleState::kNotRunning, std::memory_order_release);
    return std::optional<runtime::AppRunOutcome>{outcome};
}

void* AppController::RunSession(void* context) {
    auto& controller = *static_cast<AppController*>(context);
    runtime::AppRunOutcome outcome = controller.runtime_.RunApp(controller.selected_app_, SessionReady, &controller);
    if (controller.stop_requested_.load(std::memory_order_acquire) &&
        outcome.completion == runtime::AppCompletion::kFailed) {
        outcome.completion = runtime::AppCompletion::kStopped;
    }
    controller.state_.store(AppLifecycleState::kNotRunning, std::memory_order_release);
    (void)xQueueOverwrite(controller.completion_queue_, &outcome);
    return nullptr;
}

void AppController::SessionReady(void* context) {
    auto& controller = *static_cast<AppController*>(context);
    if (controller.stop_requested_.load(std::memory_order_acquire)) {
        controller.state_.store(AppLifecycleState::kStopping, std::memory_order_release);
        (void)controller.runtime_.RequestStop();
        return;
    }
    AppLifecycleState expected = AppLifecycleState::kStarting;
    (void)controller.state_.compare_exchange_strong(expected, AppLifecycleState::kForeground,
                                                    std::memory_order_acq_rel);
}

}  // namespace micropixel::firmware
