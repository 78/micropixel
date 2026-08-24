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
    ui_.LeaveStatusLayer();
    ui_.LeaveWifiSettings();
    ui_.LeaveAppManagement();
    ui_.LeaveSystemInformation();
    ui_.LeaveSystemMenu();
    ui_.LeaveHall();
}

std::expected<void, SystemUiError> SystemShell::ShowHall(const HallModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    (void)xQueueReset(action_queue_);
    return ui_.ShowHall(model, ReceiveAction, this);
}

void SystemShell::UpdateHallWifi(const HallWifiModel& model) { ui_.UpdateHallWifi(model); }

std::optional<SystemUiAction> SystemShell::PollAction(TickType_t timeout) {
    SystemUiAction action{};
    if (action_queue_ == nullptr || xQueueReceive(action_queue_, &action, timeout) != pdTRUE) {
        return std::nullopt;
    }
    return action;
}

void SystemShell::LeaveHall() { ui_.LeaveHall(); }

std::expected<void, SystemUiError> SystemShell::RestoreGuestView() { return ui_.RestoreGuestView(); }

void SystemShell::WatchGuestActions() {
    if (action_queue_ == nullptr) {
        return;
    }
    (void)xQueueReset(action_queue_);
    ui_.WatchGuestActions(ReceiveAction, this);
}

void SystemShell::StopWatchingGuestActions() { ui_.StopWatchingGuestActions(this); }

std::expected<HallCoverModel, SystemUiError> SystemShell::CaptureGuestFrame() { return ui_.CaptureGuestFrame(); }

void SystemShell::ReleaseGuestSnapshot() { ui_.ReleaseGuestSnapshot(); }

std::expected<void, SystemUiError> SystemShell::ShowSystemMenu(const SystemMenuModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    (void)xQueueReset(action_queue_);
    return ui_.ShowSystemMenu(model, ReceiveAction, this);
}

void SystemShell::LeaveSystemMenu() { ui_.LeaveSystemMenu(); }

std::expected<void, SystemUiError> SystemShell::ShowSystemInformation(const SystemInformationModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    (void)xQueueReset(action_queue_);
    return ui_.ShowSystemInformation(model, ReceiveAction, this);
}

void SystemShell::LeaveSystemInformation() { ui_.LeaveSystemInformation(); }

std::expected<void, SystemUiError> SystemShell::ShowAppManagement(const AppManagementModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    (void)xQueueReset(action_queue_);
    return ui_.ShowAppManagement(model, ReceiveAction, this);
}

void SystemShell::LeaveAppManagement() { ui_.LeaveAppManagement(); }

std::expected<void, SystemUiError> SystemShell::ShowWifiSettings(const WifiSettingsModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    (void)xQueueReset(action_queue_);
    return ui_.ShowWifiSettings(model, ReceiveAction, this);
}

void SystemShell::UpdateWifiSettings(const WifiSettingsModel& model) { ui_.UpdateWifiSettings(model); }

void SystemShell::LeaveWifiSettings() { ui_.LeaveWifiSettings(); }

std::expected<void, SystemUiError> SystemShell::ShowStatusLayer(const StatusLayerModel& model) {
    if (action_queue_ == nullptr) {
        return std::unexpected(SystemUiError::kUnavailable);
    }
    (void)xQueueReset(action_queue_);
    return ui_.ShowStatusLayer(model, ReceiveAction, this);
}

void SystemShell::UpdateStatusLayer(const StatusLayerModel& model) { ui_.UpdateStatusLayer(model); }

void SystemShell::LeaveStatusLayer() { ui_.LeaveStatusLayer(); }

void SystemShell::UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) {
    ui_.UpdatePerformanceOverlay(enabled, cpu_percent);
}

void SystemShell::ApplyBrightness(uint8_t percent) { ui_.ApplyBrightness(percent); }

void SystemShell::ApplyVolume(uint8_t percent) { ui_.ApplyVolume(percent); }

void SystemShell::ReceiveAction(void* context, const SystemUiAction& action) {
    auto* shell = static_cast<SystemShell*>(context);
    if (shell != nullptr && shell->action_queue_ != nullptr) {
        if (xQueueSend(shell->action_queue_, &action, 0U) != pdTRUE) {
            ESP_LOGW(kTag, "System action queue full; dropped action=%u", static_cast<unsigned>(action.type));
        }
    }
}

}  // namespace micropixel::host_ui
