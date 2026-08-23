#include <cstdlib>
#include <expected>
#include <iostream>

#include "host_ui/system_shell.hpp"

namespace {

using micropixel::host_ui::HallCoverModel;
using micropixel::host_ui::HallModel;
using micropixel::host_ui::StatusLayerModel;
using micropixel::host_ui::SystemUiAction;
using micropixel::host_ui::SystemUiActionSink;
using micropixel::host_ui::SystemUiActionType;
using micropixel::host_ui::SystemUiBackend;
using micropixel::host_ui::SystemUiError;
using micropixel::host_ui::SystemShell;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeSystemUi final : public SystemUiBackend {
   public:
    std::expected<void, SystemUiError> ShowHall(const HallModel&, SystemUiActionSink sink,
                                                void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void LeaveHall() override { ++leave_hall_calls; }
    std::expected<void, SystemUiError> RestoreGuestView() override { return {}; }

    void WatchGuestActions(SystemUiActionSink sink, void* context) override {
        sink_ = sink;
        context_ = context;
    }

    void StopWatchingGuestActions(void* context) override {
        ++stop_watching_calls;
        if (context_ == context) {
            sink_ = nullptr;
            context_ = nullptr;
        }
    }

    std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame() override { return HallCoverModel{}; }
    void ReleaseGuestSnapshot() override {}

    std::expected<void, SystemUiError> ShowStatusLayer(const StatusLayerModel&, SystemUiActionSink sink,
                                                       void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void UpdateStatusLayer(const StatusLayerModel&) override {}
    void LeaveStatusLayer() override { ++leave_status_calls; }
    void UpdatePerformanceOverlay(bool, uint8_t) override {}
    void ApplyBrightness(uint8_t) override {}
    void ApplyVolume(uint8_t) override {}

    void Emit(const SystemUiAction& action) const {
        if (sink_ != nullptr) {
            sink_(context_, action);
        }
    }

    [[nodiscard]] bool HasSink() const { return sink_ != nullptr || context_ != nullptr; }

    uint32_t stop_watching_calls{};
    uint32_t leave_status_calls{};
    uint32_t leave_hall_calls{};

   private:
    SystemUiActionSink sink_{};
    void* context_{};
};

void DiscreteActionsRemainOrdered() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowHall(HallModel{}).has_value(), "hall should render");

    ui.Emit({.type = SystemUiActionType::kLaunchApp, .app_index = 2U});
    ui.Emit({.type = SystemUiActionType::kOpenStatusLayer});
    ui.Emit({.type = SystemUiActionType::kStopApp, .app_index = 1U});

    const auto first = shell.PollAction(0U);
    const auto second = shell.PollAction(0U);
    const auto third = shell.PollAction(0U);
    Check(first.has_value() && first->type == SystemUiActionType::kLaunchApp && first->app_index == 2U,
          "first action should be retained");
    Check(second.has_value() && second->type == SystemUiActionType::kOpenStatusLayer,
          "second action should be retained");
    Check(third.has_value() && third->type == SystemUiActionType::kStopApp && third->app_index == 1U,
          "third action should be retained");
    Check(!shell.PollAction(0U).has_value(), "queue should be empty after three receives");
}

void DestructorUnbindsCallbacks() {
    FakeSystemUi ui;
    {
        SystemShell shell(ui);
        Check(shell.ShowHall(HallModel{}).has_value(), "hall should render");
        Check(ui.HasSink(), "backend should hold shell callback while active");
    }

    Check(!ui.HasSink(), "shell destructor should remove backend callback");
    Check(ui.stop_watching_calls == 1U, "shell destructor should stop guest action watching");
    Check(ui.leave_status_calls == 1U, "shell destructor should leave status layer");
    Check(ui.leave_hall_calls == 1U, "shell destructor should leave hall");
}

}  // namespace

int main() {
    DiscreteActionsRemainOrdered();
    DestructorUnbindsCallbacks();
    std::cout << "system_shell tests passed: 2 cases\n";
    return 0;
}
