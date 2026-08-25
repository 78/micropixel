#include <cstdlib>
#include <expected>
#include <iostream>

#include "host_ui/system_shell.hpp"

namespace {

using micropixel::host_ui::AppManagementModel;
using micropixel::host_ui::HallCoverModel;
using micropixel::host_ui::HallModel;
using micropixel::host_ui::RemoteControlModel;
using micropixel::host_ui::StatusLayerModel;
using micropixel::host_ui::SystemInformationModel;
using micropixel::host_ui::SystemMenuModel;
using micropixel::host_ui::SystemShell;
using micropixel::host_ui::SystemUiAction;
using micropixel::host_ui::SystemUiActionSink;
using micropixel::host_ui::SystemUiActionType;
using micropixel::host_ui::SystemUiBackend;
using micropixel::host_ui::SystemUiError;
using micropixel::host_ui::WifiSettingsModel;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeSystemUi final : public SystemUiBackend {
   public:
    std::expected<void, SystemUiError> ShowHall(const HallModel&, SystemUiActionSink sink, void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void UpdateHallStatusBar(const micropixel::host_ui::HallStatusBarModel&) override {
        ++update_hall_status_bar_calls;
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

    std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame(uint32_t, uint64_t) override {
        return HallCoverModel{};
    }
    void ReleaseGuestSnapshot() override {}

    std::expected<void, SystemUiError> ShowSystemMenu(const SystemMenuModel&, SystemUiActionSink sink,
                                                      void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void UpdateSystemMenu(const SystemMenuModel&) override { ++update_system_menu_calls; }
    void LeaveSystemMenu() override { ++leave_system_menu_calls; }

    std::expected<void, SystemUiError> ShowSystemInformation(const SystemInformationModel&, SystemUiActionSink sink,
                                                             void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void LeaveSystemInformation() override { ++leave_system_information_calls; }

    std::expected<void, SystemUiError> ShowRemoteControl(const RemoteControlModel&, SystemUiActionSink sink,
                                                         void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void UpdateRemoteControl(const RemoteControlModel&) override { ++update_remote_control_calls; }
    void LeaveRemoteControl() override { ++leave_remote_control_calls; }

    std::expected<void, SystemUiError> ShowAppManagement(const AppManagementModel&, SystemUiActionSink sink,
                                                         void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void LeaveAppManagement() override { ++leave_app_management_calls; }

    std::expected<void, SystemUiError> ShowWifiSettings(const WifiSettingsModel&, SystemUiActionSink sink,
                                                        void* context) override {
        sink_ = sink;
        context_ = context;
        return {};
    }

    void UpdateWifiSettings(const WifiSettingsModel&) override { ++update_wifi_settings_calls; }
    void LeaveWifiSettings() override { ++leave_wifi_settings_calls; }

    std::expected<void, SystemUiError> ShowStatusLayer(const StatusLayerModel&, uint64_t trigger_timestamp_us,
                                                       SystemUiActionSink sink, void* context) override {
        status_open_trigger_us = trigger_timestamp_us;
        sink_ = sink;
        context_ = context;
        return {};
    }

    void UpdateStatusLayer(const StatusLayerModel&) override {}
    void LeaveStatusLayer(uint64_t trigger_timestamp_us) override {
        status_close_trigger_us = trigger_timestamp_us;
        ++leave_status_calls;
    }
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
    uint64_t status_open_trigger_us{};
    uint64_t status_close_trigger_us{};
    uint32_t update_hall_status_bar_calls{};
    uint32_t update_system_menu_calls{};
    uint32_t update_remote_control_calls{};
    uint32_t leave_remote_control_calls{};
    uint32_t leave_system_menu_calls{};
    uint32_t leave_system_information_calls{};
    uint32_t leave_app_management_calls{};
    uint32_t update_wifi_settings_calls{};
    uint32_t leave_wifi_settings_calls{};
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
    Check(ui.leave_system_menu_calls == 1U, "shell destructor should leave system menu");
    Check(ui.leave_system_information_calls == 1U, "shell destructor should leave system information");
    Check(ui.leave_app_management_calls == 1U, "shell destructor should leave App Management");
    Check(ui.leave_wifi_settings_calls == 1U, "shell destructor should leave Wi-Fi settings");
    Check(ui.leave_hall_calls == 1U, "shell destructor should leave hall");
}

void SystemMenuActionsReachTheShell() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowSystemMenu(SystemMenuModel{}).has_value(), "system menu should render");

    ui.Emit({.type = SystemUiActionType::kSelectSystemMenuItem,
             .value = static_cast<uint32_t>(micropixel::host_ui::SystemMenuItem::kWifi)});
    ui.Emit({.type = SystemUiActionType::kCloseSystemMenu});

    const auto selected = shell.PollAction(0U);
    const auto closed = shell.PollAction(0U);
    Check(selected.has_value() && selected->type == SystemUiActionType::kSelectSystemMenuItem &&
              selected->value == static_cast<uint32_t>(micropixel::host_ui::SystemMenuItem::kWifi),
          "system menu selection should be retained");
    Check(closed.has_value() && closed->type == SystemUiActionType::kCloseSystemMenu,
          "system menu close should be retained");

    shell.UpdateSystemMenu(SystemMenuModel{});
    Check(ui.update_system_menu_calls == 1U, "system menu updates should reach the backend");
}

void HallStatusBarUpdatesReachTheShell() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowHall(HallModel{}).has_value(), "hall should render before a status-bar update");

    shell.UpdateHallStatusBar(micropixel::host_ui::HallStatusBarModel{});
    Check(ui.update_hall_status_bar_calls == 1U, "Hall status-bar updates should reach the backend");
}

void WifiStateNotificationsAreCoalescedAndSurviveScreenChanges() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowHall(HallModel{}).has_value(), "hall should render");

    shell.NotifyWifiStateChanged();
    shell.NotifyWifiStateChanged();
    const auto coalesced = shell.PollAction(0U);
    Check(coalesced.has_value() && coalesced->type == SystemUiActionType::kWifiStateChanged,
          "duplicate Wi-Fi state notifications should coalesce");
    Check(!shell.PollAction(0U).has_value(), "coalesced Wi-Fi notification should only be queued once");

    shell.NotifyWifiStateChanged();
    Check(shell.ShowSystemMenu(SystemMenuModel{}).has_value(), "system menu should render after Wi-Fi notification");
    const auto preserved = shell.PollAction(0U);
    Check(preserved.has_value() && preserved->type == SystemUiActionType::kWifiStateChanged,
          "pending Wi-Fi notification should survive a screen queue reset");
}

void BatteryStateNotificationsAreCoalescedAndSurviveScreenChanges() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowStatusLayer(StatusLayerModel{}).has_value(), "status layer should render");

    shell.NotifyBatteryStateChanged();
    shell.NotifyBatteryStateChanged();
    const auto coalesced = shell.PollAction(0U);
    Check(coalesced.has_value() && coalesced->type == SystemUiActionType::kBatteryStateChanged,
          "duplicate Battery state notifications should coalesce");
    Check(!shell.PollAction(0U).has_value(), "coalesced Battery notification should only be queued once");

    shell.NotifyBatteryStateChanged();
    Check(shell.ShowStatusLayer(StatusLayerModel{}).has_value(),
          "status layer should rerender after Battery notification");
    const auto preserved = shell.PollAction(0U);
    Check(preserved.has_value() && preserved->type == SystemUiActionType::kBatteryStateChanged,
          "pending Battery notification should survive a screen queue reset");
}

void WifiActionsReachTheShell() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowWifiSettings(WifiSettingsModel{}).has_value(), "Wi-Fi settings should render");

    SystemUiAction action{.type = SystemUiActionType::kConnectNewWifi};
    action.text[0] = 'A';
    action.secret[0] = 'B';
    ui.Emit(action);
    const auto received = shell.PollAction(0U);
    Check(received.has_value() && received->type == SystemUiActionType::kConnectNewWifi && received->text[0] == 'A' &&
              received->secret[0] == 'B',
          "Wi-Fi credentials should be retained in the action queue");

    shell.UpdateWifiSettings(WifiSettingsModel{});
    Check(ui.update_wifi_settings_calls == 1U, "Wi-Fi updates should reach the backend");
}

void DetailScreenActionsReachTheShell() {
    FakeSystemUi ui;
    SystemShell shell(ui);
    Check(shell.ShowSystemInformation(SystemInformationModel{}).has_value(), "system information should render");
    ui.Emit({.type = SystemUiActionType::kCloseSystemInformation});
    const auto close_info = shell.PollAction(0U);
    Check(close_info.has_value() && close_info->type == SystemUiActionType::kCloseSystemInformation,
          "system information close should be retained");

    Check(shell.ShowRemoteControl(RemoteControlModel{}).has_value(), "Remote Control should render");
    ui.Emit({.type = SystemUiActionType::kSetRemoteControlEnabled, .value = 1U});
    const auto enable_remote = shell.PollAction(0U);
    Check(enable_remote.has_value() && enable_remote->type == SystemUiActionType::kSetRemoteControlEnabled &&
              enable_remote->value == 1U,
          "Remote Control actions should be retained");
    shell.UpdateRemoteControl(RemoteControlModel{});
    Check(ui.update_remote_control_calls == 1U, "Remote Control updates should reach the backend");

    Check(shell.ShowAppManagement(AppManagementModel{}).has_value(), "App Management should render");
    ui.Emit({.type = SystemUiActionType::kLaunchInstalledApp, .app_index = 1U});
    const auto launch = shell.PollAction(0U);
    Check(launch.has_value() && launch->type == SystemUiActionType::kLaunchInstalledApp && launch->app_index == 1U,
          "installed App launch should be retained");
}

}  // namespace

int main() {
    DiscreteActionsRemainOrdered();
    DestructorUnbindsCallbacks();
    SystemMenuActionsReachTheShell();
    HallStatusBarUpdatesReachTheShell();
    WifiStateNotificationsAreCoalescedAndSurviveScreenChanges();
    BatteryStateNotificationsAreCoalescedAndSurviveScreenChanges();
    WifiActionsReachTheShell();
    DetailScreenActionsReachTheShell();
    std::cout << "system_shell tests passed: 8 cases\n";
    return 0;
}
