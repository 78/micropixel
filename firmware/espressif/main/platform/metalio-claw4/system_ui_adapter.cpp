#include "platform/metalio-claw4/system_ui_adapter.hpp"

namespace micropixel::platform::metalio_claw4 {

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowHall(const host_ui::HallModel& model,
                                                                      host_ui::SystemUiActionSink action_sink,
                                                                      void* action_context) {
    return operations_.show_hall(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdateHallStatusBar(const host_ui::HallStatusBarModel& model) {
    operations_.update_hall_status_bar(operations_.context, model);
}

void SystemUiAdapter::PauseHallCoverLoading() {
    if (operations_.pause_hall_cover_loading != nullptr) {
        operations_.pause_hall_cover_loading(operations_.context);
    }
}

void SystemUiAdapter::LeaveHall() { operations_.leave_hall(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::RestoreGuestView() {
    return operations_.restore_guest_view(operations_.context);
}

void SystemUiAdapter::WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) {
    operations_.watch_guest_actions(operations_.context, action_sink, action_context);
}

void SystemUiAdapter::StopWatchingGuestActions(void* action_context) {
    operations_.stop_watching_guest_actions(operations_.context, action_context);
}

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> SystemUiAdapter::CaptureGuestFrame(
    uint32_t hall_app_index, uint64_t trigger_timestamp_us) {
    return operations_.capture_guest_frame(operations_.context, hall_app_index, trigger_timestamp_us);
}

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> SystemUiAdapter::CaptureScreenJpeg() {
    if (operations_.capture_screen_jpeg == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    return operations_.capture_screen_jpeg(operations_.context);
}

void SystemUiAdapter::ReleaseGuestSnapshot() { operations_.release_guest_snapshot(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                            host_ui::SystemUiActionSink action_sink,
                                                                            void* action_context) {
    return operations_.show_system_menu(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdateSystemMenu(const host_ui::SystemMenuModel& model) {
    operations_.update_system_menu(operations_.context, model);
}

void SystemUiAdapter::LeaveSystemMenu() { operations_.leave_system_menu(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowSystemInformation(
    const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    return operations_.show_system_information(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdateSystemInformation(const host_ui::SystemInformationModel& model) {
    if (operations_.update_system_information != nullptr) {
        operations_.update_system_information(operations_.context, model);
    }
}

void SystemUiAdapter::LeaveSystemInformation() { operations_.leave_system_information(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowPowerManagement(
    const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    return operations_.show_power_management(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdatePowerManagement(const host_ui::PowerManagementModel& model) {
    operations_.update_power_management(operations_.context, model);
}

void SystemUiAdapter::LeavePowerManagement() { operations_.leave_power_management(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowRemoteControl(const host_ui::RemoteControlModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context) {
    return operations_.show_remote_control(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdateRemoteControl(const host_ui::RemoteControlModel& model) {
    operations_.update_remote_control(operations_.context, model);
}

void SystemUiAdapter::LeaveRemoteControl() { operations_.leave_remote_control(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowAppManagement(const host_ui::AppManagementModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context) {
    return operations_.show_app_management(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::LeaveAppManagement() { operations_.leave_app_management(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) {
    return operations_.show_wifi_settings(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdateWifiSettings(const host_ui::WifiSettingsModel& model) {
    operations_.update_wifi_settings(operations_.context, model);
}

void SystemUiAdapter::LeaveWifiSettings() { operations_.leave_wifi_settings(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                             uint64_t trigger_timestamp_us,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) {
    return operations_.show_status_layer(operations_.context, model, trigger_timestamp_us, action_sink, action_context);
}

void SystemUiAdapter::UpdateStatusLayer(const host_ui::StatusLayerModel& model) {
    operations_.update_status_layer(operations_.context, model);
}

void SystemUiAdapter::LeaveStatusLayer(uint64_t trigger_timestamp_us) {
    operations_.leave_status_layer(operations_.context, trigger_timestamp_us);
}

void SystemUiAdapter::UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) {
    operations_.update_performance_overlay(operations_.context, enabled, cpu_percent);
}

void SystemUiAdapter::ApplyBrightness(uint8_t percent) { operations_.apply_brightness(operations_.context, percent); }

void SystemUiAdapter::ApplyVolume(uint8_t percent) { operations_.apply_volume(operations_.context, percent); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowShutdown() {
    return operations_.show_shutdown(operations_.context);
}

}  // namespace micropixel::platform::metalio_claw4
