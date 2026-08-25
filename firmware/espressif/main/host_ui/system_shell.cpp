#include "host_ui/system_shell.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace micropixel::host_ui {
namespace {

constexpr char kTag[] = "micropixel_shell";

}  // namespace

SystemShell::SystemShell(SystemUiBackend& ui) : ui_(ui) {
    action_queue_ = xQueueCreateStatic(kActionQueueCapacity, sizeof(SystemUiAction), action_queue_bytes_.data(),
                                       &action_queue_storage_);
}

SystemShell::~SystemShell() {
    ui_.StopWatchingGuestActions(this);
    ui_.LeaveStatusLayer(0U);
    ui_.LeaveWifiSettings();
    ui_.LeaveAppManagement();
    ui_.LeaveRemoteControl();
    ui_.LeaveSystemInformation();
    ui_.LeaveSystemMenu();
    ui_.LeaveHall();
}

std::expected<void, SystemUiError> SystemShell::ShowHall(const HallModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowHall(model, ReceiveAction, this);
}

void SystemShell::UpdateHallStatusBar(const HallStatusBarModel& model) { ui_.UpdateHallStatusBar(model); }

std::optional<SystemUiAction> SystemShell::PollAction(TickType_t timeout) {
    SystemUiAction action{};
    if (action_queue_ == nullptr || xQueueReceive(action_queue_, &action, timeout) != pdTRUE) {
        return std::nullopt;
    }
    if (action.type == SystemUiActionType::kWifiStateChanged) {
        wifi_state_change_pending_.store(false, std::memory_order_release);
        wifi_state_change_queued_.store(false, std::memory_order_release);
    } else if (action.type == SystemUiActionType::kBatteryStateChanged) {
        battery_state_change_pending_.store(false, std::memory_order_release);
        battery_state_change_queued_.store(false, std::memory_order_release);
    }
    QueuePendingWifiStateChange();
    QueuePendingBatteryStateChange();
    return action;
}

void SystemShell::LeaveHall() { ui_.LeaveHall(); }

std::expected<void, SystemUiError> SystemShell::RestoreGuestView() { return ui_.RestoreGuestView(); }

void SystemShell::WatchGuestActions() {
    if (action_queue_ == nullptr) {
        return;
    }
    ResetActionQueue();
    ui_.WatchGuestActions(ReceiveAction, this);
}

void SystemShell::StopWatchingGuestActions() { ui_.StopWatchingGuestActions(this); }

std::expected<HallCoverModel, SystemUiError> SystemShell::CaptureGuestFrame(uint32_t hall_app_index,
                                                                            uint64_t trigger_timestamp_us) {
    return ui_.CaptureGuestFrame(hall_app_index, trigger_timestamp_us);
}

std::expected<ScreenCapture, SystemUiError> SystemShell::CaptureScreenJpeg() { return ui_.CaptureScreenJpeg(); }

void SystemShell::ReleaseGuestSnapshot() { ui_.ReleaseGuestSnapshot(); }

std::expected<void, SystemUiError> SystemShell::ShowSystemMenu(const SystemMenuModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowSystemMenu(model, ReceiveAction, this);
}

void SystemShell::UpdateSystemMenu(const SystemMenuModel& model) { ui_.UpdateSystemMenu(model); }

void SystemShell::LeaveSystemMenu() { ui_.LeaveSystemMenu(); }

std::expected<void, SystemUiError> SystemShell::ShowSystemInformation(const SystemInformationModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowSystemInformation(model, ReceiveAction, this);
}

void SystemShell::UpdateSystemInformation(const SystemInformationModel& model) { ui_.UpdateSystemInformation(model); }

void SystemShell::LeaveSystemInformation() { ui_.LeaveSystemInformation(); }

std::expected<void, SystemUiError> SystemShell::ShowRemoteControl(const RemoteControlModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowRemoteControl(model, ReceiveAction, this);
}

void SystemShell::UpdateRemoteControl(const RemoteControlModel& model) { ui_.UpdateRemoteControl(model); }

void SystemShell::LeaveRemoteControl() { ui_.LeaveRemoteControl(); }

std::expected<void, SystemUiError> SystemShell::ShowAppManagement(const AppManagementModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowAppManagement(model, ReceiveAction, this);
}

void SystemShell::LeaveAppManagement() { ui_.LeaveAppManagement(); }

std::expected<void, SystemUiError> SystemShell::ShowWifiSettings(const WifiSettingsModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowWifiSettings(model, ReceiveAction, this);
}

void SystemShell::UpdateWifiSettings(const WifiSettingsModel& model) { ui_.UpdateWifiSettings(model); }

void SystemShell::LeaveWifiSettings() { ui_.LeaveWifiSettings(); }

std::expected<void, SystemUiError> SystemShell::ShowStatusLayer(const StatusLayerModel& model,
                                                                uint64_t trigger_timestamp_us) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowStatusLayer(model, trigger_timestamp_us, ReceiveAction, this);
}

void SystemShell::UpdateStatusLayer(const StatusLayerModel& model) { ui_.UpdateStatusLayer(model); }

void SystemShell::LeaveStatusLayer(uint64_t trigger_timestamp_us) { ui_.LeaveStatusLayer(trigger_timestamp_us); }

void SystemShell::UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) {
    ui_.UpdatePerformanceOverlay(enabled, cpu_percent);
}

void SystemShell::ApplyBrightness(uint8_t percent) { ui_.ApplyBrightness(percent); }

void SystemShell::ApplyVolume(uint8_t percent) { ui_.ApplyVolume(percent); }

void SystemShell::NotifyWifiStateChanged() {
    if (action_queue_ == nullptr) {
        return;
    }
    wifi_state_change_pending_.store(true, std::memory_order_release);
    QueuePendingWifiStateChange();
}

void SystemShell::NotifyBatteryStateChanged() {
    if (action_queue_ == nullptr) {
        return;
    }
    battery_state_change_pending_.store(true, std::memory_order_release);
    QueuePendingBatteryStateChange();
}

void SystemShell::QueuePendingWifiStateChange() {
    if (action_queue_ == nullptr || !wifi_state_change_pending_.load(std::memory_order_acquire) ||
        wifi_state_change_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const SystemUiAction action{.type = SystemUiActionType::kWifiStateChanged};
    if (xQueueSend(action_queue_, &action, 0U) != pdTRUE) {
        wifi_state_change_queued_.store(false, std::memory_order_release);
    }
}

void SystemShell::QueuePendingBatteryStateChange() {
    if (action_queue_ == nullptr || !battery_state_change_pending_.load(std::memory_order_acquire) ||
        battery_state_change_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const SystemUiAction action{.type = SystemUiActionType::kBatteryStateChanged};
    if (xQueueSend(action_queue_, &action, 0U) != pdTRUE) {
        battery_state_change_queued_.store(false, std::memory_order_release);
    }
}

void SystemShell::ResetActionQueue() {
    if (action_queue_ == nullptr) {
        return;
    }
    (void)xQueueReset(action_queue_);
    wifi_state_change_queued_.store(false, std::memory_order_release);
    battery_state_change_queued_.store(false, std::memory_order_release);
    QueuePendingWifiStateChange();
    QueuePendingBatteryStateChange();
}

void SystemShell::ReceiveAction(void* context, const SystemUiAction& action) {
    auto* shell = static_cast<SystemShell*>(context);
    if (shell != nullptr && shell->action_queue_ != nullptr) {
        if (xQueueSend(shell->action_queue_, &action, 0U) != pdTRUE) {
            ESP_LOGW(kTag, "System action queue full; dropped action=%u", static_cast<unsigned>(action.type));
        }
    }
}

}  // namespace micropixel::host_ui
