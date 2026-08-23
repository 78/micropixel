#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>

#include "app_controller.hpp"

namespace {

using micropixel::firmware::AppController;
using micropixel::firmware::AppControllerError;
using micropixel::firmware::AppLifecycleState;
using micropixel::runtime::AppCompletion;
using micropixel::runtime::AppRunOutcome;
using micropixel::runtime::AppRuntime;
using micropixel::runtime::InstalledApp;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
    }
    return condition;
}

InstalledApp TestApp(const char* id) {
    InstalledApp app;
    (void)std::snprintf(app.app_id.data(), app.app_id.size(), "%s", id);
    return app;
}

bool WaitForState(const AppController& controller, AppLifecycleState expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        if (controller.state() == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return controller.state() == expected;
}

std::optional<AppRunOutcome> WaitForCompletion(AppController& controller) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = controller.PollCompletion(pdMS_TO_TICKS(10));
        if (!result) {
            return std::nullopt;
        }
        if (result->has_value()) {
            return **result;
        }
    }
    return std::nullopt;
}

bool ForegroundSuspendResumeStop() {
    AppRuntime runtime;
    AppController controller(runtime);
    const InstalledApp app = TestApp("micropixel.lifecycle");

    auto start = controller.Start(app);
    if (!Check(start.has_value(), "Start must accept the first App") ||
        !Check(runtime.WaitUntilStarted(), "Guest worker must enter RunApp")) {
        return false;
    }
    runtime.AllowReady();
    if (!Check(WaitForState(controller, AppLifecycleState::kForeground), "ready session must become Foreground")) {
        return false;
    }

    auto suspend = controller.Suspend(pdMS_TO_TICKS(100));
    auto resume = controller.Resume();
    if (!Check(suspend.has_value(), "Foreground App must suspend") ||
        !Check(controller.state() == AppLifecycleState::kForeground, "resumed App must return to Foreground") ||
        !Check(resume.has_value(), "Suspended App must resume") ||
        !Check(runtime.suspend_requests() == 1U && runtime.resume_requests() == 1U,
               "controller must issue one suspend and one resume request")) {
        return false;
    }

    if (!Check(controller.RequestStop(), "Foreground App must accept stop")) {
        return false;
    }
    const auto outcome = WaitForCompletion(controller);
    return Check(outcome.has_value(), "stopped App must publish completion") &&
           Check(outcome->completion == AppCompletion::kStopped, "stop completion must be classified as Stopped") &&
           Check(controller.state() == AppLifecycleState::kNotRunning, "joined stopped App must finish in NotRunning");
}

bool SuspendedAppCanStopWithoutResuming() {
    AppRuntime runtime;
    AppController controller(runtime);
    const InstalledApp app = TestApp("micropixel.hall-close");

    if (!controller.Start(app) || !runtime.WaitUntilStarted()) {
        return Check(false, "Hall-close test App must start");
    }
    runtime.AllowReady();
    if (!Check(WaitForState(controller, AppLifecycleState::kForeground),
               "Hall-close test App must reach Foreground") ||
        !Check(controller.Suspend(pdMS_TO_TICKS(100)).has_value(), "Hall-close test App must suspend") ||
        !Check(controller.state() == AppLifecycleState::kSuspended, "Hall-close target must remain Suspended") ||
        !Check(controller.RequestStop(), "Suspended App must accept Hall stop")) {
        return false;
    }

    const auto outcome = WaitForCompletion(controller);
    return Check(outcome.has_value(), "Hall-stopped App must publish completion") &&
           Check(outcome->completion == AppCompletion::kStopped, "Hall stop completion must be Stopped") &&
           Check(runtime.resume_requests() == 0U, "Hall stop must not resume the Guest") &&
           Check(controller.state() == AppLifecycleState::kNotRunning,
                 "Hall-stopped App must finish in NotRunning");
}

bool InvalidTransitionsAreRejected() {
    AppRuntime runtime;
    AppController controller(runtime);
    auto suspend = controller.Suspend(pdMS_TO_TICKS(1));
    auto resume = controller.Resume();
    return Check(!suspend && suspend.error() == AppControllerError::kInvalidState, "NotRunning cannot suspend") &&
           Check(!resume && resume.error() == AppControllerError::kInvalidState, "NotRunning cannot resume") &&
           Check(!controller.RequestStop(), "NotRunning cannot stop");
}

bool CompletionWinsSuspendRaceAndAllowsRestart() {
    AppRuntime runtime;
    AppController controller(runtime);
    const InstalledApp app = TestApp("micropixel.race");
    if (!controller.Start(app) || !runtime.WaitUntilStarted()) {
        return Check(false, "race test App must start");
    }
    runtime.AllowReady();
    if (!WaitForState(controller, AppLifecycleState::kForeground)) {
        return Check(false, "race test App must reach Foreground");
    }

    runtime.SetSuspendBehavior(AppRuntime::SuspendBehavior::kCompleteThenFail);
    auto suspend = controller.Suspend(pdMS_TO_TICKS(100));
    if (!Check(!suspend && suspend.error() == AppControllerError::kSuspendTimeout,
               "completion during suspend must report the failed suspend") ||
        !Check(WaitForCompletion(controller).has_value(), "racing completion must remain joinable") ||
        !Check(controller.state() == AppLifecycleState::kNotRunning,
               "completion must be the final lifecycle arbitration point")) {
        return false;
    }

    runtime.PrepareNextRun();
    if (!Check(controller.Start(app).has_value(), "controller must be reusable after the suspend/completion race") ||
        !Check(runtime.WaitUntilStarted(), "restarted Guest must enter RunApp")) {
        return false;
    }
    runtime.AllowReady();
    if (!WaitForState(controller, AppLifecycleState::kForeground)) {
        return Check(false, "restarted Guest must reach Foreground");
    }
    (void)controller.RequestStop();
    return Check(WaitForCompletion(controller).has_value(), "restarted Guest must stop cleanly");
}

bool StopWhileStartingCompletesCleanly() {
    AppRuntime runtime;
    AppController controller(runtime);
    const InstalledApp app = TestApp("micropixel.starting");
    if (!controller.Start(app) || !runtime.WaitUntilStarted()) {
        return Check(false, "starting-stop test App must start");
    }
    if (!Check(controller.RequestStop(), "Starting App must accept stop")) {
        return false;
    }
    const auto outcome = WaitForCompletion(controller);
    return Check(outcome.has_value(), "Starting App stop must publish completion") &&
           Check(outcome->completion == AppCompletion::kStopped, "Starting App stop must be Stopped") &&
           Check(controller.state() == AppLifecycleState::kNotRunning, "Starting App stop must reach NotRunning");
}

bool UncooperativeStopCanBeForced() {
    AppRuntime runtime;
    AppController controller(runtime);
    const InstalledApp app = TestApp("micropixel.uncooperative");
    runtime.IgnoreCooperativeStop(true);
    if (!controller.Start(app) || !runtime.WaitUntilStarted()) {
        return Check(false, "uncooperative App must start");
    }
    runtime.AllowReady();
    if (!WaitForState(controller, AppLifecycleState::kForeground) || !controller.RequestStop()) {
        return Check(false, "uncooperative foreground App must enter Stopping");
    }
    auto premature = controller.PollCompletion(pdMS_TO_TICKS(20));
    if (!Check(premature.has_value() && !premature->has_value(),
               "cooperative Stop must not fabricate completion when Guest ignores it") ||
        !Check(controller.ForceStop(), "Host must retain a forced-stop fallback")) {
        return false;
    }
    const auto outcome = WaitForCompletion(controller);
    return Check(outcome.has_value() && outcome->completion == AppCompletion::kStopped,
                 "forced fallback must finish as Stopped") &&
           Check(runtime.stop_requests() == 1U && runtime.force_stop_requests() == 1U,
                 "fallback must issue one cooperative and one forced request");
}

bool ForcedStopHasFinalDeadline() {
    AppRuntime runtime;
    AppController controller(runtime);
    const InstalledApp app = TestApp("micropixel.stuck");
    runtime.IgnoreCooperativeStop(true);
    runtime.IgnoreForcedStop(true);
    if (!controller.Start(app) || !runtime.WaitUntilStarted()) {
        return Check(false, "stuck App must start");
    }
    runtime.AllowReady();
    if (!WaitForState(controller, AppLifecycleState::kForeground)) {
        return Check(false, "stuck App must reach Foreground");
    }

    auto stopped = controller.Stop(pdMS_TO_TICKS(10), pdMS_TO_TICKS(30));
    if (!Check(!stopped && stopped.error() == AppControllerError::kTerminationTimeout,
               "unresponsive forced stop must return a bounded timeout") ||
        !Check(runtime.force_stop_requests() > 0U, "bounded stop must attempt forced termination")) {
        return false;
    }

    runtime.IgnoreForcedStop(false);
    if (!Check(controller.ForceStop(), "test cleanup must be able to release the stuck worker")) {
        return false;
    }
    return Check(WaitForCompletion(controller).has_value(), "stuck test worker must remain joinable after timeout");
}

}  // namespace

int main() {
    const bool passed = ForegroundSuspendResumeStop() && SuspendedAppCanStopWithoutResuming() &&
                        InvalidTransitionsAreRejected() &&
                        CompletionWinsSuspendRaceAndAllowsRestart() && StopWhileStartingCompletesCleanly() &&
                        UncooperativeStopCanBeForced() && ForcedStopHasFinalDeadline();
    if (!passed) {
        return 1;
    }
    std::puts("AppController host tests passed (7 cases).");
    return 0;
}
