#ifndef MICROPIXEL_RUNTIME_APP_RUNTIME_HPP
#define MICROPIXEL_RUNTIME_APP_RUNTIME_HPP

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

#include "abi/micropixel_abi.h"
#include "freertos/FreeRTOS.h"

namespace micropixel::runtime {

constexpr size_t kTestAppIdLength = 64U;

struct InstalledApp final {
    std::array<char, kTestAppIdLength + 1U> app_id{};
    uint32_t store_offset{};
    uint32_t bundle_size{};
};

enum class AppSessionError {
    kPackageLoad,
};

enum class AppCompletion {
    kExited,
    kStopped,
    kFailed,
};

struct AppRunOutcome final {
    std::array<char, kTestAppIdLength + 1U> app_id{};
    AppCompletion completion{AppCompletion::kFailed};
    AppSessionError error{AppSessionError::kPackageLoad};
};

using AppSessionReadySink = void (*)(void* context);

class AppRuntime final {
   public:
    enum class SuspendBehavior {
        kSucceed,
        kCompleteThenFail,
    };

    [[nodiscard]] AppRunOutcome RunApp(const InstalledApp& app,
                                       const micropixel_system_launch_arguments_response_t& launch_arguments,
                                       AppSessionReadySink ready_sink = nullptr, void* ready_context = nullptr) {
        {
            std::lock_guard lock(mutex_);
            launch_arguments_ = launch_arguments;
            active_ = true;
            started_ = true;
            returned_ = false;
        }
        changed_.notify_all();

        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return ready_allowed_ || stop_requested_ || finish_requested_; });
        }
        if (ready_allowed_ && ready_sink != nullptr) {
            ready_sink(ready_context);
        }

        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return stop_requested_ || finish_requested_; });
        AppRunOutcome outcome;
        (void)std::snprintf(outcome.app_id.data(), outcome.app_id.size(), "%s", app.app_id.data());
        outcome.completion = stop_requested_ ? AppCompletion::kStopped : AppCompletion::kExited;
        active_ = false;
        returned_ = true;
        lock.unlock();
        changed_.notify_all();
        return outcome;
    }

    [[nodiscard]] bool RequestSuspend(TickType_t) {
        std::unique_lock lock(mutex_);
        ++suspend_requests_;
        if (!active_) {
            return false;
        }
        if (suspend_behavior_ == SuspendBehavior::kCompleteThenFail) {
            finish_requested_ = true;
            lock.unlock();
            changed_.notify_all();
            lock.lock();
            changed_.wait(lock, [this] { return returned_; });
            lock.unlock();
            // RunApp has returned, allowing the production worker to publish
            // kNotRunning before the failed request writes kStopping.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return false;
        }
        suspended_ = suspend_behavior_ == SuspendBehavior::kSucceed;
        return suspended_;
    }

    [[nodiscard]] bool RequestResume() {
        std::lock_guard lock(mutex_);
        ++resume_requests_;
        if (!active_ || !suspended_) {
            return false;
        }
        suspended_ = false;
        return true;
    }

    [[nodiscard]] bool RequestStop() {
        {
            std::lock_guard lock(mutex_);
            ++stop_requests_;
            if (!ignore_cooperative_stop_) {
                stop_requested_ = true;
            }
        }
        changed_.notify_all();
        return true;
    }

    [[nodiscard]] bool ForceStop() {
        {
            std::lock_guard lock(mutex_);
            ++force_stop_requests_;
            if (!ignore_forced_stop_) {
                stop_requested_ = true;
            }
        }
        changed_.notify_all();
        return !ignore_forced_stop_;
    }

    void IgnoreCooperativeStop(bool ignore) {
        std::lock_guard lock(mutex_);
        ignore_cooperative_stop_ = ignore;
    }

    void IgnoreForcedStop(bool ignore) {
        std::lock_guard lock(mutex_);
        ignore_forced_stop_ = ignore;
    }

    void AllowReady() {
        {
            std::lock_guard lock(mutex_);
            ready_allowed_ = true;
        }
        changed_.notify_all();
    }

    void PrepareNextRun() {
        std::lock_guard lock(mutex_);
        started_ = false;
        ready_allowed_ = false;
        stop_requested_ = false;
        finish_requested_ = false;
        returned_ = false;
        suspended_ = false;
        suspend_behavior_ = SuspendBehavior::kSucceed;
    }

    [[nodiscard]] bool WaitUntilStarted(std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return started_; });
    }

    void SetSuspendBehavior(SuspendBehavior behavior) {
        std::lock_guard lock(mutex_);
        suspend_behavior_ = behavior;
    }

    [[nodiscard]] uint32_t suspend_requests() const {
        std::lock_guard lock(mutex_);
        return suspend_requests_;
    }

    [[nodiscard]] uint32_t resume_requests() const {
        std::lock_guard lock(mutex_);
        return resume_requests_;
    }

    [[nodiscard]] uint32_t stop_requests() const {
        std::lock_guard lock(mutex_);
        return stop_requests_;
    }

    [[nodiscard]] uint32_t force_stop_requests() const {
        std::lock_guard lock(mutex_);
        return force_stop_requests_;
    }

    [[nodiscard]] micropixel_system_launch_arguments_response_t launch_arguments() const {
        std::lock_guard lock(mutex_);
        return launch_arguments_;
    }

   private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    SuspendBehavior suspend_behavior_{SuspendBehavior::kSucceed};
    uint32_t suspend_requests_{};
    uint32_t resume_requests_{};
    uint32_t stop_requests_{};
    uint32_t force_stop_requests_{};
    micropixel_system_launch_arguments_response_t launch_arguments_{};
    bool active_{};
    bool started_{};
    bool ready_allowed_{};
    bool stop_requested_{};
    bool finish_requested_{};
    bool returned_{};
    bool ignore_cooperative_stop_{};
    bool ignore_forced_stop_{};
    bool suspended_{};
};

}  // namespace micropixel::runtime

#endif
