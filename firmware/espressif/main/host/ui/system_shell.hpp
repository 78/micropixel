#ifndef MICROPIXEL_HOST_UI_SYSTEM_SHELL_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_SHELL_HPP

#include <array>
#include <atomic>
#include <expected>
#include <optional>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui {

// Runs on the Host supervisor task. Returns whether external-power state is
// available and writes the current connection state when it is.
using ExternalPowerStateQuery = bool (*)(void* context, bool& connected);

// Long-lived Host owner for the App Hall and its action queue.
class SystemShell final {
   public:
    explicit SystemShell(SystemUi& ui);
    SystemShell(const SystemShell&) = delete;
    SystemShell& operator=(const SystemShell&) = delete;
    ~SystemShell();

    [[nodiscard]] std::expected<void, SystemUiError> ShowHall(const HallModel& model);
    void UpdateHallStatusBar(const HallStatusBarModel& model);
    void UpdateHallInstallProgress(uint32_t app_index, uint8_t progress_percent);
    void PauseHallCoverLoading();
    void PrepareAppLaunch(uint32_t app_index);
    [[nodiscard]] std::optional<SystemUiAction> PollAction(TickType_t timeout);
    void LeaveHall();
    [[nodiscard]] std::expected<void, SystemUiError> RestoreGuestView();
    void WatchGuestActions();
    void StopWatchingGuestActions();
    [[nodiscard]] std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame(uint32_t hall_app_index,
                                                                                 uint64_t trigger_timestamp_us);
    [[nodiscard]] std::expected<ScreenCapture, SystemUiError> CaptureScreenJpeg();
    [[nodiscard]] bool SupportsScreenCapture() const;
    void ReleaseGuestSnapshot();
    [[nodiscard]] std::expected<void, SystemUiError> ShowSystemMenu(const SystemMenuModel& model);
    void UpdateSystemMenu(const SystemMenuModel& model);
    void LeaveSystemMenu();
    void ApplyTheme(SystemThemeMode mode);
    [[nodiscard]] std::expected<void, SystemUiError> ShowSystemInformation(const SystemInformationModel& model);
    void UpdateSystemInformation(const SystemInformationModel& model);
    void LeaveSystemInformation();
    [[nodiscard]] std::expected<void, SystemUiError> ShowPowerManagement(const PowerManagementModel& model);
    void UpdatePowerManagement(const PowerManagementModel& model);
    void LeavePowerManagement();
    [[nodiscard]] std::expected<void, SystemUiError> ShowAppearance(const AppearanceModel& model);
    void UpdateAppearance(const AppearanceModel& model);
    void LeaveAppearance();
    [[nodiscard]] std::expected<void, SystemUiError> ShowRemoteControl(const RemoteControlModel& model);
    void UpdateRemoteControl(const RemoteControlModel& model);
    void LeaveRemoteControl();
    [[nodiscard]] std::expected<void, SystemUiError> ShowAppManagement(const AppManagementModel& model);
    void LeaveAppManagement();
    [[nodiscard]] std::expected<void, SystemUiError> ShowWifiSettings(const WifiSettingsModel& model);
    void UpdateWifiSettings(const WifiSettingsModel& model);
    void LeaveWifiSettings();
    [[nodiscard]] std::expected<void, SystemUiError> ShowStatusLayer(const StatusLayerModel& model,
                                                                     uint64_t trigger_timestamp_us = 0U);
    void UpdateStatusLayer(const StatusLayerModel& model);
    void LeaveStatusLayer(uint64_t trigger_timestamp_us = 0U);
    void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent);
    void ApplyBrightness(uint8_t percent);
    void ApplyVolume(uint8_t percent);
    [[nodiscard]] std::expected<void, SystemUiError> ShowShutdown();
    [[nodiscard]] bool NotifyPowerButtonPressed(uint64_t timestamp_us);
    [[nodiscard]] bool PowerButtonPressed() const;
    [[nodiscard]] bool ConsumePowerButtonPressed();
    [[nodiscard]] bool NotifyPowerOffRequested(uint64_t timestamp_us);
    [[nodiscard]] bool PowerOffRequested() const;
    [[nodiscard]] bool ConsumePowerOffRequested();
    [[nodiscard]] bool PowerTransitionRequested() const;
    void NotifyWifiStateChanged();
    void NotifyBatteryStateChanged();
    void NotifyTimeStateChanged();
    void NotifyRemoteCommandReady();
    void NotifyUserActivity();
    void ConfigureAutoSleep(uint8_t timeout_minutes, ExternalPowerStateQuery power_query, void* power_context);
    void SetAutoSleepTimeout(uint8_t timeout_minutes);
    void NotifyPowerCycleCompleted();

   private:
    static constexpr UBaseType_t kActionQueueCapacity = 10U;

    static void ReceiveAction(void* context, const SystemUiAction& action);
    void QueuePendingPowerButton();
    void QueuePendingPowerOff();
    void QueuePendingWifiStateChange();
    void QueuePendingBatteryStateChange();
    void QueuePendingTimeStateChange();
    void QueuePendingRemoteCommand();
    void QueuePendingUserActivity();
    void RefreshExternalPowerState();
    [[nodiscard]] TickType_t AutoSleepAwareTimeout(TickType_t requested_timeout) const;
    [[nodiscard]] bool RequestAutoSleepIfDue();
    void RecordUserActivity();
    void ResetActionQueue();

    SystemUi& ui_;
    StaticQueue_t action_queue_storage_{};
    std::array<uint8_t, sizeof(SystemUiAction) * kActionQueueCapacity> action_queue_bytes_{};
    QueueHandle_t action_queue_{};
    std::atomic_bool power_transition_pending_{};
    std::atomic_bool power_button_pending_{};
    std::atomic_bool power_button_queued_{};
    std::atomic<uint64_t> power_button_timestamp_us_{};
    std::atomic_bool power_off_pending_{};
    std::atomic_bool power_off_queued_{};
    std::atomic<uint64_t> power_off_timestamp_us_{};
    std::atomic_bool wifi_state_change_pending_{};
    std::atomic_bool wifi_state_change_queued_{};
    std::atomic_bool battery_state_change_pending_{};
    std::atomic_bool battery_state_change_queued_{};
    std::atomic_bool time_state_change_pending_{};
    std::atomic_bool time_state_change_queued_{};
    std::atomic_bool remote_command_pending_{};
    std::atomic_bool remote_command_queued_{};
    std::atomic_bool user_activity_pending_{};
    std::atomic_bool user_activity_queued_{};
    std::atomic<TickType_t> last_user_activity_ticks_{};
    std::atomic<uint8_t> auto_sleep_timeout_minutes_{};
    ExternalPowerStateQuery external_power_query_{};
    void* external_power_context_{};
    bool external_power_connected_{};
    bool external_power_available_{};
};

}  // namespace micropixel::host_ui

#endif
