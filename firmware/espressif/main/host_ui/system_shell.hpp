#ifndef MICROPIXEL_HOST_UI_SYSTEM_SHELL_HPP
#define MICROPIXEL_HOST_UI_SYSTEM_SHELL_HPP

#include <array>
#include <atomic>
#include <expected>
#include <optional>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host_ui/system_ui.hpp"

namespace micropixel::host_ui {

// Long-lived Host owner for the App Hall and its action queue.
class SystemShell final {
   public:
    explicit SystemShell(SystemUiBackend& ui);
    SystemShell(const SystemShell&) = delete;
    SystemShell& operator=(const SystemShell&) = delete;
    ~SystemShell();

    [[nodiscard]] std::expected<void, SystemUiError> ShowHall(const HallModel& model);
    void UpdateHallWifi(const HallWifiModel& model);
    [[nodiscard]] std::optional<SystemUiAction> PollAction(TickType_t timeout);
    void LeaveHall();
    [[nodiscard]] std::expected<void, SystemUiError> RestoreGuestView();
    void WatchGuestActions();
    void StopWatchingGuestActions();
    [[nodiscard]] std::expected<HallCoverModel, SystemUiError> CaptureGuestFrame(uint32_t hall_app_index,
                                                                                 uint64_t trigger_timestamp_us);
    void ReleaseGuestSnapshot();
    [[nodiscard]] std::expected<void, SystemUiError> ShowSystemMenu(const SystemMenuModel& model);
    void UpdateSystemMenu(const SystemMenuModel& model);
    void LeaveSystemMenu();
    [[nodiscard]] std::expected<void, SystemUiError> ShowSystemInformation(const SystemInformationModel& model);
    void LeaveSystemInformation();
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
    void NotifyWifiStateChanged();

   private:
    static constexpr UBaseType_t kActionQueueCapacity = 8U;

    static void ReceiveAction(void* context, const SystemUiAction& action);
    void QueuePendingWifiStateChange();
    void ResetActionQueue();

    SystemUiBackend& ui_;
    StaticQueue_t action_queue_storage_{};
    std::array<uint8_t, sizeof(SystemUiAction) * kActionQueueCapacity> action_queue_bytes_{};
    QueueHandle_t action_queue_{};
    std::atomic_bool wifi_state_change_pending_{};
    std::atomic_bool wifi_state_change_queued_{};
};

}  // namespace micropixel::host_ui

#endif
