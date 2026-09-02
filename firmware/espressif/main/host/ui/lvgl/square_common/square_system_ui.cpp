#include "host/ui/lvgl/square_common/square_system_ui.hpp"

#include "esp_lv_adapter.h"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {

SquareSystemUi::SquareSystemUi(SquareSystemUiState& state, SquarePresentation& presentation)
    : state_(state), presentation_(presentation), hall_policy_(state_, presentation_) {}

SquareSystemUi::~SquareSystemUi() = default;

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowHall(const host_ui::HallModel& model,
                                                                     host_ui::SystemUiActionSink action_sink,
                                                                     void* action_context) {
    return hall_policy_.Show(model, action_sink, action_context);
}

void SquareSystemUi::UpdateHallStatusBar(const host_ui::HallStatusBarModel& model) {
    hall_policy_.UpdateStatusBar(model);
}

void SquareSystemUi::UpdateHallInstallProgress(uint32_t app_index, uint8_t progress_percent) {
    hall_policy_.UpdateInstallProgress(app_index, progress_percent);
}

void SquareSystemUi::PauseHallCoverLoading() { hall_policy_.PauseCoverLoading(); }

void SquareSystemUi::PrepareAppLaunch(uint32_t app_index) { hall_policy_.PrepareLaunch(app_index); }

void SquareSystemUi::LeaveHall() { hall_policy_.Leave(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::RestoreGuestView() {
    uint32_t running_index = host_ui::kMaxHallApps;
    for (uint32_t index = 0U; index < state_.hall_app_count; ++index) {
        if (state_.hall_app_running[index]) {
            running_index = index;
            break;
        }
    }
    const HallTransitionPresentation presentation =
        HallTransitionPresentationFor(state_.profile, running_index, state_.hall_scroll_offset);
    state_.UnbindHostPointerTouchSink();
    state_.input_router.ClearSystemActionSink(state_.hall_action_context);
    state_.hall_action_sink = nullptr;
    state_.hall_action_context = nullptr;
    state_.hall_app_count = 0U;
    state_.hall_launch_enabled = false;
    lv_obj_t* guest_frame = state_.GuestFrameLocked();
    if (state_.display == nullptr || state_.root == nullptr || guest_frame == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    DisplayTransition* transition = presentation_.Transition();
    const bool transitioned =
        transition != nullptr && transition->AnimateToGuest(state_.root, guest_frame, presentation,
                                                            state_.profile.square.guest_transition_duration_ms);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kRenderFailed);
    }
    state_.SetHostPointerEnabledLocked(false);
    lv_obj_delete(state_.root);
    state_.root = nullptr;
    state_.launch_image_descriptor = {};
    lv_obj_move_foreground(guest_frame);
    lv_obj_invalidate(guest_frame);
    platform::lvgl::RequestDisplayRefresh(state_.display);
    esp_lv_adapter_unlock();
    if (transition != nullptr && transition->SynchronizeGuestReveal() &&
        state_.GuestRefreshSynchronizationAvailable()) {
        state_.WaitForGuestRefreshReady();
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        state_.ResetHallPresentationLocked();
        esp_lv_adapter_unlock();
    }
    (void)transitioned;
    return {};
}

void SquareSystemUi::WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) {
    state_.WatchGuestActions(action_sink, action_context);
}

void SquareSystemUi::StopWatchingGuestActions(void* action_context) { state_.StopWatchingGuestActions(action_context); }

std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> SquareSystemUi::CaptureGuestFrame(
    uint32_t hall_app_index, uint64_t trigger_timestamp_us) {
    DisplayTransition* transition = presentation_.Transition();
    if (transition == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    const HallTransitionPresentation presentation =
        HallTransitionPresentationFor(state_.profile, hall_app_index, state_.hall_scroll_offset);
    return transition->CaptureGuestFrame(presentation, trigger_timestamp_us,
                                         state_.profile.square.guest_transition_duration_ms);
}

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> SquareSystemUi::CaptureScreenJpeg() {
    ScreenCapture* capture = presentation_.Capture();
    if (capture == nullptr) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    return capture->CaptureScreenJpeg();
}

bool SquareSystemUi::SupportsScreenCapture() const { return presentation_.Capture() != nullptr; }

void SquareSystemUi::ReleaseGuestSnapshot() {
    if (DisplayTransition* transition = presentation_.Transition(); transition != nullptr) {
        transition->ReleaseGuestSnapshot();
    }
}

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                           host_ui::SystemUiActionSink action_sink,
                                                                           void* action_context) {
    return state_.ShowSystemMenu(model, action_sink, action_context);
}

void SquareSystemUi::UpdateSystemMenu(const host_ui::SystemMenuModel& model) { state_.UpdateSystemMenu(model); }

void SquareSystemUi::LeaveSystemMenu() { state_.LeaveSystemMenu(); }

void SquareSystemUi::ApplyTheme(host_ui::SystemThemeMode mode) { state_.ApplyTheme(mode); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowSystemInformation(
    const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    return state_.ShowSystemInformation(model, action_sink, action_context);
}

void SquareSystemUi::UpdateSystemInformation(const host_ui::SystemInformationModel& model) {
    state_.UpdateSystemInformation(model);
}

void SquareSystemUi::LeaveSystemInformation() { state_.LeaveSystemInformation(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowPowerManagement(
    const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink, void* action_context) {
    return state_.ShowPowerManagement(model, action_sink, action_context);
}

void SquareSystemUi::UpdatePowerManagement(const host_ui::PowerManagementModel& model) {
    state_.UpdatePowerManagement(model);
}

void SquareSystemUi::LeavePowerManagement() { state_.LeavePowerManagement(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowAppearance(const host_ui::AppearanceModel& model,
                                                                           host_ui::SystemUiActionSink action_sink,
                                                                           void* action_context) {
    return state_.ShowAppearance(model, action_sink, action_context);
}

void SquareSystemUi::UpdateAppearance(const host_ui::AppearanceModel& model) { state_.UpdateAppearance(model); }

void SquareSystemUi::LeaveAppearance() { state_.LeaveAppearance(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowRemoteControl(const host_ui::RemoteControlModel& model,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) {
    return state_.ShowRemoteControl(model, action_sink, action_context);
}

void SquareSystemUi::UpdateRemoteControl(const host_ui::RemoteControlModel& model) {
    state_.UpdateRemoteControl(model);
}

void SquareSystemUi::LeaveRemoteControl() { state_.LeaveRemoteControl(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowAppManagement(const host_ui::AppManagementModel& model,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) {
    return state_.ShowAppManagement(model, action_sink, action_context);
}

void SquareSystemUi::LeaveAppManagement() { state_.LeaveAppManagement(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) {
    return state_.ShowWifiSettings(model, action_sink, action_context);
}

void SquareSystemUi::UpdateWifiSettings(const host_ui::WifiSettingsModel& model) { state_.UpdateWifiSettings(model); }

void SquareSystemUi::LeaveWifiSettings() { state_.LeaveWifiSettings(); }

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                            uint64_t trigger_timestamp_us,
                                                                            host_ui::SystemUiActionSink action_sink,
                                                                            void* action_context) {
    return state_.ShowStatusLayer(model, trigger_timestamp_us, action_sink, action_context);
}

void SquareSystemUi::UpdateStatusLayer(const host_ui::StatusLayerModel& model) { state_.UpdateStatusLayer(model); }

void SquareSystemUi::LeaveStatusLayer(uint64_t trigger_timestamp_us) { state_.LeaveStatusLayer(trigger_timestamp_us); }

void SquareSystemUi::UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) {
    state_.UpdatePerformanceOverlay(enabled, cpu_percent);
}

void SquareSystemUi::ApplyBrightness(uint8_t percent) {
    if (BrightnessControl* control = presentation_.Brightness(); control != nullptr) {
        control->ApplyBrightness(percent);
    }
}

void SquareSystemUi::ApplyVolume(uint8_t percent) {
    if (VolumeControl* control = presentation_.Volume(); control != nullptr) {
        control->ApplyVolume(percent);
    }
}

std::expected<void, host_ui::SystemUiError> SquareSystemUi::ShowShutdown() {
    return state_.ShowShutdown(PrepareShutdownLocked, this);
}

void SquareSystemUi::PrepareShutdownLocked(void* context) {
    auto& ui = *static_cast<SquareSystemUi*>(context);
    if (ShutdownPresentation* presentation = ui.presentation_.Shutdown(); presentation != nullptr) {
        presentation->PrepareShutdownLocked();
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
