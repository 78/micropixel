#include "host/ui/system_shell.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::host_ui {
namespace {

constexpr char kTag[] = "micropixel_shell";
constexpr uint32_t kMillisecondsPerMinute = 60U * 1000U;

}  // namespace

SystemShell::SystemShell(SystemUi& ui) : ui_(ui) {
    action_queue_ = xQueueCreateStatic(kActionQueueCapacity, sizeof(SystemUiAction), action_queue_bytes_.data(),
                                       &action_queue_storage_);
    RecordUserActivity();
}

SystemShell::~SystemShell() {
    ui_.StopWatchingGuestActions(this);
    ui_.LeaveStatusLayer(0U);
    ui_.LeaveWifiSettings();
    ui_.LeaveAppManagement();
    ui_.LeaveRemoteControl();
    ui_.LeaveSystemInformation();
    ui_.LeavePowerManagement();
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

void SystemShell::UpdateHallInstallProgress(uint32_t app_index, uint8_t progress_percent) {
    ui_.UpdateHallInstallProgress(app_index, progress_percent);
}

std::optional<SystemUiAction> SystemShell::PollAction(TickType_t timeout) {
    TickType_t receive_timeout = AutoSleepAwareTimeout(timeout);
    for (;;) {
        SystemUiAction action{};
        if (action_queue_ == nullptr || xQueueReceive(action_queue_, &action, receive_timeout) != pdTRUE) {
            if (RequestAutoSleepIfDue()) {
                receive_timeout = 0U;
                continue;
            }
            return std::nullopt;
        }
        if (action.type == SystemUiActionType::kPowerButtonPressed) {
            power_button_queued_.store(false, std::memory_order_release);
            if (!power_button_pending_.load(std::memory_order_acquire)) {
                receive_timeout = 0U;
                continue;
            }
            action.timestamp_us = power_button_timestamp_us_.load(std::memory_order_acquire);
        } else if (action.type == SystemUiActionType::kPowerOffRequested) {
            power_off_queued_.store(false, std::memory_order_release);
            if (!power_off_pending_.load(std::memory_order_acquire)) {
                receive_timeout = 0U;
                continue;
            }
            action.timestamp_us = power_off_timestamp_us_.load(std::memory_order_acquire);
        } else if (action.type == SystemUiActionType::kWifiStateChanged) {
            wifi_state_change_pending_.store(false, std::memory_order_release);
            wifi_state_change_queued_.store(false, std::memory_order_release);
        } else if (action.type == SystemUiActionType::kBatteryStateChanged) {
            battery_state_change_pending_.store(false, std::memory_order_release);
            battery_state_change_queued_.store(false, std::memory_order_release);
            RefreshExternalPowerState();
        } else if (action.type == SystemUiActionType::kRemoteCommandReady) {
            remote_command_pending_.store(false, std::memory_order_release);
            remote_command_queued_.store(false, std::memory_order_release);
        } else if (action.type == SystemUiActionType::kUserActivity) {
            user_activity_pending_.store(false, std::memory_order_release);
            user_activity_queued_.store(false, std::memory_order_release);
        }
        QueuePendingWifiStateChange();
        QueuePendingBatteryStateChange();
        QueuePendingRemoteCommand();
        QueuePendingUserActivity();
        if (action.type != SystemUiActionType::kPowerButtonPressed) {
            QueuePendingPowerButton();
        }
        if (action.type != SystemUiActionType::kPowerOffRequested) {
            QueuePendingPowerOff();
        }
        if (action.type == SystemUiActionType::kRemoteCommandReady ||
            action.type == SystemUiActionType::kUserActivity) {
            return std::nullopt;
        }
        return action;
    }
}

void SystemShell::LeaveHall() { ui_.LeaveHall(); }

void SystemShell::PauseHallCoverLoading() { ui_.PauseHallCoverLoading(); }

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

bool SystemShell::SupportsScreenCapture() const { return ui_.SupportsScreenCapture(); }

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

void SystemShell::ApplyTheme(SystemThemeMode mode) { ui_.ApplyTheme(mode); }

std::expected<void, SystemUiError> SystemShell::ShowSystemInformation(const SystemInformationModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowSystemInformation(model, ReceiveAction, this);
}

void SystemShell::UpdateSystemInformation(const SystemInformationModel& model) { ui_.UpdateSystemInformation(model); }

void SystemShell::LeaveSystemInformation() { ui_.LeaveSystemInformation(); }

std::expected<void, SystemUiError> SystemShell::ShowPowerManagement(const PowerManagementModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowPowerManagement(model, ReceiveAction, this);
}

void SystemShell::UpdatePowerManagement(const PowerManagementModel& model) { ui_.UpdatePowerManagement(model); }

void SystemShell::LeavePowerManagement() { ui_.LeavePowerManagement(); }

std::expected<void, SystemUiError> SystemShell::ShowAppearance(const AppearanceModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    ResetActionQueue();
    return ui_.ShowAppearance(model, ReceiveAction, this);
}

void SystemShell::UpdateAppearance(const AppearanceModel& model) { ui_.UpdateAppearance(model); }

void SystemShell::LeaveAppearance() { ui_.LeaveAppearance(); }

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

std::expected<void, SystemUiError> SystemShell::ShowShutdown() {
    ui_.StopWatchingGuestActions(this);
    return ui_.ShowShutdown();
}

bool SystemShell::NotifyPowerButtonPressed(uint64_t timestamp_us) {
    if (action_queue_ == nullptr) {
        return false;
    }
    bool expected = false;
    if (!power_transition_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    power_button_timestamp_us_.store(timestamp_us, std::memory_order_release);
    power_button_pending_.store(true, std::memory_order_release);
    QueuePendingPowerButton();
    return true;
}

bool SystemShell::NotifyPowerOffRequested(uint64_t timestamp_us) {
    if (action_queue_ == nullptr) {
        return false;
    }
    bool expected = false;
    if (!power_transition_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    power_off_timestamp_us_.store(timestamp_us, std::memory_order_release);
    power_off_pending_.store(true, std::memory_order_release);
    QueuePendingPowerOff();
    return true;
}

bool SystemShell::PowerOffRequested() const { return power_off_pending_.load(std::memory_order_acquire); }

bool SystemShell::ConsumePowerOffRequested() {
    const bool pending = power_off_pending_.exchange(false, std::memory_order_acq_rel);
    power_off_queued_.store(false, std::memory_order_release);
    if (pending) {
        power_transition_pending_.store(false, std::memory_order_release);
    }
    QueuePendingPowerOff();
    return pending;
}

bool SystemShell::PowerTransitionRequested() const { return power_transition_pending_.load(std::memory_order_acquire); }

bool SystemShell::PowerButtonPressed() const { return power_button_pending_.load(std::memory_order_acquire); }

bool SystemShell::ConsumePowerButtonPressed() {
    const bool pending = power_button_pending_.exchange(false, std::memory_order_acq_rel);
    power_button_queued_.store(false, std::memory_order_release);
    if (pending) {
        power_transition_pending_.store(false, std::memory_order_release);
    }
    QueuePendingPowerButton();
    return pending;
}

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

void SystemShell::NotifyRemoteCommandReady() {
    if (action_queue_ == nullptr) {
        return;
    }
    remote_command_pending_.store(true, std::memory_order_release);
    QueuePendingRemoteCommand();
}

void SystemShell::NotifyUserActivity() {
    if (action_queue_ == nullptr) {
        return;
    }
    RecordUserActivity();
    user_activity_pending_.store(true, std::memory_order_release);
    QueuePendingUserActivity();
}

void SystemShell::ConfigureAutoSleep(uint8_t timeout_minutes, ExternalPowerStateQuery power_query,
                                     void* power_context) {
    external_power_query_ = power_query;
    external_power_context_ = power_context;
    SetAutoSleepTimeout(timeout_minutes);
    RefreshExternalPowerState();
}

void SystemShell::SetAutoSleepTimeout(uint8_t timeout_minutes) {
    auto_sleep_timeout_minutes_.store(timeout_minutes, std::memory_order_release);
    RecordUserActivity();
}

void SystemShell::NotifyPowerCycleCompleted() { RecordUserActivity(); }

void SystemShell::QueuePendingPowerButton() {
    if (action_queue_ == nullptr || !power_button_pending_.load(std::memory_order_acquire) ||
        power_button_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const SystemUiAction action{
        .type = SystemUiActionType::kPowerButtonPressed,
        .timestamp_us = power_button_timestamp_us_.load(std::memory_order_acquire),
    };
    if (xQueueSend(action_queue_, &action, 0U) != pdTRUE) {
        power_button_queued_.store(false, std::memory_order_release);
    }
}

void SystemShell::QueuePendingPowerOff() {
    if (action_queue_ == nullptr || !power_off_pending_.load(std::memory_order_acquire) ||
        power_off_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const SystemUiAction action{
        .type = SystemUiActionType::kPowerOffRequested,
        .timestamp_us = power_off_timestamp_us_.load(std::memory_order_acquire),
    };
    if (xQueueSend(action_queue_, &action, 0U) != pdTRUE) {
        power_off_queued_.store(false, std::memory_order_release);
    }
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

void SystemShell::QueuePendingRemoteCommand() {
    if (action_queue_ == nullptr || !remote_command_pending_.load(std::memory_order_acquire) ||
        remote_command_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const SystemUiAction action{.type = SystemUiActionType::kRemoteCommandReady};
    if (xQueueSend(action_queue_, &action, 0U) != pdTRUE) {
        remote_command_queued_.store(false, std::memory_order_release);
    }
}

void SystemShell::QueuePendingUserActivity() {
    if (action_queue_ == nullptr || !user_activity_pending_.load(std::memory_order_acquire) ||
        user_activity_queued_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const SystemUiAction action{.type = SystemUiActionType::kUserActivity};
    if (xQueueSend(action_queue_, &action, 0U) != pdTRUE) {
        user_activity_queued_.store(false, std::memory_order_release);
    }
}

void SystemShell::RefreshExternalPowerState() {
    bool connected = false;
    const bool available =
        external_power_query_ != nullptr && external_power_query_(external_power_context_, connected);
    const bool began_running_on_battery =
        available && !connected && (!external_power_available_ || external_power_connected_);
    external_power_available_ = available;
    external_power_connected_ = connected;
    if (began_running_on_battery) {
        RecordUserActivity();
    }
}

TickType_t SystemShell::AutoSleepAwareTimeout(TickType_t requested_timeout) const {
    const uint8_t timeout_minutes = auto_sleep_timeout_minutes_.load(std::memory_order_acquire);
    if (timeout_minutes == 0U || !external_power_available_ || external_power_connected_) {
        return requested_timeout;
    }
    const TickType_t timeout_ticks = pdMS_TO_TICKS(static_cast<uint32_t>(timeout_minutes) * kMillisecondsPerMinute);
    const TickType_t elapsed_ticks = xTaskGetTickCount() - last_user_activity_ticks_.load(std::memory_order_acquire);
    if (elapsed_ticks >= timeout_ticks) {
        return 0U;
    }
    const TickType_t remaining_ticks = timeout_ticks - elapsed_ticks;
    return requested_timeout < remaining_ticks ? requested_timeout : remaining_ticks;
}

bool SystemShell::RequestAutoSleepIfDue() {
    const uint8_t timeout_minutes = auto_sleep_timeout_minutes_.load(std::memory_order_acquire);
    if (timeout_minutes == 0U || !external_power_available_ || external_power_connected_) {
        return false;
    }
    const TickType_t timeout_ticks = pdMS_TO_TICKS(static_cast<uint32_t>(timeout_minutes) * kMillisecondsPerMinute);
    const TickType_t now_ticks = xTaskGetTickCount();
    const TickType_t elapsed_ticks = now_ticks - last_user_activity_ticks_.load(std::memory_order_acquire);
    if (elapsed_ticks < timeout_ticks) {
        return false;
    }
    if (NotifyPowerButtonPressed(static_cast<uint64_t>(now_ticks) * portTICK_PERIOD_MS * 1000U)) {
        ESP_LOGI(kTag, "auto sleep requested after %u minutes without interaction",
                 static_cast<unsigned>(timeout_minutes));
    }
    return PowerTransitionRequested();
}

void SystemShell::RecordUserActivity() {
    last_user_activity_ticks_.store(xTaskGetTickCount(), std::memory_order_release);
}

void SystemShell::ResetActionQueue() {
    if (action_queue_ == nullptr) {
        return;
    }
    (void)xQueueReset(action_queue_);
    power_button_queued_.store(false, std::memory_order_release);
    power_off_queued_.store(false, std::memory_order_release);
    wifi_state_change_queued_.store(false, std::memory_order_release);
    battery_state_change_queued_.store(false, std::memory_order_release);
    remote_command_queued_.store(false, std::memory_order_release);
    user_activity_queued_.store(false, std::memory_order_release);
    QueuePendingPowerButton();
    QueuePendingPowerOff();
    QueuePendingWifiStateChange();
    QueuePendingBatteryStateChange();
    QueuePendingRemoteCommand();
    QueuePendingUserActivity();
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
