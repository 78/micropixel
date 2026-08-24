#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_SYSTEM_UI_ADAPTER_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_SYSTEM_UI_ADAPTER_HPP

#include "host_ui/system_ui.hpp"

namespace micropixel::platform::metalio_claw4 {

// Board rendering stays private to platform.cpp. This operation table keeps
// the Host-facing SystemUiBackend adapter independent from graphics/device
// backend implementation details.
struct SystemUiOperations final {
    void* context{};
    std::expected<void, host_ui::SystemUiError> (*show_hall)(void*, const host_ui::HallModel&,
                                                             host_ui::SystemUiActionSink, void*){};
    void (*update_hall_wifi)(void*, const host_ui::HallWifiModel&){};
    void (*leave_hall)(void*){};
    std::expected<void, host_ui::SystemUiError> (*restore_guest_view)(void*){};
    void (*watch_guest_actions)(void*, host_ui::SystemUiActionSink, void*){};
    void (*stop_watching_guest_actions)(void*, void*){};
    std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> (*capture_guest_frame)(void*, uint32_t, uint64_t){};
    void (*release_guest_snapshot)(void*){};
    std::expected<void, host_ui::SystemUiError> (*show_system_menu)(void*, const host_ui::SystemMenuModel&,
                                                                    host_ui::SystemUiActionSink, void*){};
    void (*update_system_menu)(void*, const host_ui::SystemMenuModel&){};
    void (*leave_system_menu)(void*){};
    std::expected<void, host_ui::SystemUiError> (*show_system_information)(void*,
                                                                           const host_ui::SystemInformationModel&,
                                                                           host_ui::SystemUiActionSink, void*){};
    void (*leave_system_information)(void*){};
    std::expected<void, host_ui::SystemUiError> (*show_app_management)(void*, const host_ui::AppManagementModel&,
                                                                       host_ui::SystemUiActionSink, void*){};
    void (*leave_app_management)(void*){};
    std::expected<void, host_ui::SystemUiError> (*show_wifi_settings)(void*, const host_ui::WifiSettingsModel&,
                                                                      host_ui::SystemUiActionSink, void*){};
    void (*update_wifi_settings)(void*, const host_ui::WifiSettingsModel&){};
    void (*leave_wifi_settings)(void*){};
    std::expected<void, host_ui::SystemUiError> (*show_status_layer)(void*, const host_ui::StatusLayerModel&, uint64_t,
                                                                     host_ui::SystemUiActionSink, void*){};
    void (*update_status_layer)(void*, const host_ui::StatusLayerModel&){};
    void (*leave_status_layer)(void*, uint64_t){};
    void (*update_performance_overlay)(void*, bool, uint8_t){};
    void (*apply_brightness)(void*, uint8_t){};
    void (*apply_volume)(void*, uint8_t){};
};

class SystemUiAdapter final : public host_ui::SystemUiBackend {
   public:
    explicit SystemUiAdapter(SystemUiOperations operations) : operations_(operations) {}

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowHall(const host_ui::HallModel& model,
                                                                       host_ui::SystemUiActionSink action_sink,
                                                                       void* action_context) override;
    void UpdateHallWifi(const host_ui::HallWifiModel& model) override;
    void LeaveHall() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> RestoreGuestView() override;
    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) override;
    void StopWatchingGuestActions(void* action_context) override;
    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame(
        uint32_t hall_app_index, uint64_t trigger_timestamp_us) override;
    void ReleaseGuestSnapshot() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context) override;
    void UpdateSystemMenu(const host_ui::SystemMenuModel& model) override;
    void LeaveSystemMenu() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformation(
        const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context) override;
    void LeaveSystemInformation() override;
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

   private:
    SystemUiOperations operations_;
};

}  // namespace micropixel::platform::metalio_claw4

#endif
