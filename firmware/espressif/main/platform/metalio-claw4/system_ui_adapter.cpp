#include "platform/metalio-claw4/system_ui_adapter.hpp"

namespace micropixel::platform::metalio_claw4 {

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowHall(const host_ui::HallModel& model,
                                                                      host_ui::SystemUiActionSink action_sink,
                                                                      void* action_context) {
    return operations_.show_hall(operations_.context, model, action_sink, action_context);
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

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> SystemUiAdapter::CaptureGuestFrame() {
    return operations_.capture_guest_frame(operations_.context);
}

void SystemUiAdapter::ReleaseGuestSnapshot() { operations_.release_guest_snapshot(operations_.context); }

std::expected<void, host_ui::SystemUiError> SystemUiAdapter::ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) {
    return operations_.show_status_layer(operations_.context, model, action_sink, action_context);
}

void SystemUiAdapter::UpdateStatusLayer(const host_ui::StatusLayerModel& model) {
    operations_.update_status_layer(operations_.context, model);
}

void SystemUiAdapter::LeaveStatusLayer() { operations_.leave_status_layer(operations_.context); }

void SystemUiAdapter::UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) {
    operations_.update_performance_overlay(operations_.context, enabled, cpu_percent);
}

void SystemUiAdapter::ApplyBrightness(uint8_t percent) { operations_.apply_brightness(operations_.context, percent); }

void SystemUiAdapter::ApplyVolume(uint8_t percent) { operations_.apply_volume(operations_.context, percent); }

}  // namespace micropixel::platform::metalio_claw4
