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
    void (*leave_hall)(void*){};
    std::expected<void, host_ui::SystemUiError> (*restore_guest_view)(void*){};
    void (*watch_guest_actions)(void*, host_ui::SystemUiActionSink, void*){};
    void (*stop_watching_guest_actions)(void*, void*){};
    std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> (*capture_guest_frame)(void*){};
    void (*release_guest_snapshot)(void*){};
    std::expected<void, host_ui::SystemUiError> (*show_status_layer)(void*, const host_ui::StatusLayerModel&,
                                                                     host_ui::SystemUiActionSink, void*){};
    void (*update_status_layer)(void*, const host_ui::StatusLayerModel&){};
    void (*leave_status_layer)(void*){};
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
    void LeaveHall() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> RestoreGuestView() override;
    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context) override;
    void StopWatchingGuestActions(void* action_context) override;
    [[nodiscard]] std::expected<host_ui::HallCoverModel, host_ui::SystemUiError> CaptureGuestFrame() override;
    void ReleaseGuestSnapshot() override;
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context) override;
    void UpdateStatusLayer(const host_ui::StatusLayerModel& model) override;
    void LeaveStatusLayer() override;
    void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent) override;
    void ApplyBrightness(uint8_t percent) override;
    void ApplyVolume(uint8_t percent) override;

   private:
    SystemUiOperations operations_;
};

}  // namespace micropixel::platform::metalio_claw4

#endif
