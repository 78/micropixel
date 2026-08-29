#pragma once

#include "host/ui/lvgl/square_common/square_presentation.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"
#include "host/ui/lvgl/square_common/virtualized_hall_policy.hpp"
#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

class SquareSystemUi final : public host_ui::SystemUi {
   public:
    SquareSystemUi(SquareSystemUiState& state, SquarePresentation& presentation);
    ~SquareSystemUi() override;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowHall(const host_ui::HallModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context) override;
    void UpdateHallStatusBar(const host_ui::HallStatusBarModel& model) override;
    void UpdateHallInstallProgress(uint32_t app_index, uint8_t progress_percent) override;
    void PauseHallCoverLoading() override;
    void LeaveHall() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> RestoreGuestView() override;
    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) override;
    void StopWatchingGuestActions(void* action_context) override;
    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame(
        uint32_t hall_app_index, uint64_t trigger_timestamp_us) override;
    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override;
    [[nodiscard]] bool SupportsScreenCapture() const override;
    void ReleaseGuestSnapshot() override;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) override;
    void UpdateSystemMenu(const host_ui::SystemMenuModel& model) override;
    void LeaveSystemMenu() override;
    void ApplyTheme(host_ui::SystemThemeMode mode) override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformation(
        const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override;
    void UpdateSystemInformation(const host_ui::SystemInformationModel& model) override;
    void LeaveSystemInformation() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowPowerManagement(
        const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override;
    void UpdatePowerManagement(const host_ui::PowerManagementModel& model) override;
    void LeavePowerManagement() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppearance(const host_ui::AppearanceModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) override;
    void UpdateAppearance(const host_ui::AppearanceModel& model) override;
    void LeaveAppearance() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowRemoteControl(
        const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override;
    void UpdateRemoteControl(const host_ui::RemoteControlModel& model) override;
    void LeaveRemoteControl() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppManagement(
        const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override;
    void LeaveAppManagement() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context) override;
    void UpdateWifiSettings(const host_ui::WifiSettingsModel& model) override;
    void LeaveWifiSettings() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                              uint64_t trigger_timestamp_us,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) override;
    void UpdateStatusLayer(const host_ui::StatusLayerModel& model) override;
    void LeaveStatusLayer(uint64_t trigger_timestamp_us) override;
    void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) override;
    void ApplyBrightness(uint8_t percent) override;
    void ApplyVolume(uint8_t percent) override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowShutdown() override;

   private:
    static void PrepareShutdownLocked(void* context);

    SquareSystemUiState& state_;
    SquarePresentation& presentation_;
    VirtualizedHallPolicy hall_policy_;
};

}  // namespace micropixel::host_ui::lvgl::square_common
