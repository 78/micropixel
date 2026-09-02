#pragma once

#include <array>
#include <cstdint>
#include <expected>

#include "host/ui/lvgl/square_common/system_page_layout.hpp"
#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// Responsive presentation for the shared System Settings models/actions.
// The Host controller remains the single owner of navigation and mutations;
// this class only renders models using the injected layout profile and lowers
// gestures back to typed actions.
class SystemDetailUi final {
   public:
    explicit SystemDetailUi(const SystemPageLayout& layout) : layout_(layout) {}
    SystemDetailUi(const SystemDetailUi&) = delete;
    SystemDetailUi& operator=(const SystemDetailUi&) = delete;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformationLocked(
        lv_obj_t* root, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context);
    void UpdateSystemInformationLocked(const host_ui::SystemInformationModel& model);
    void LeaveSystemInformation();
    [[nodiscard]] bool SystemInformationVisible() const;
    [[nodiscard]] void* SystemInformationActionContext() const;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowPowerManagementLocked(
        lv_obj_t* root, const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context);
    void UpdatePowerManagementLocked(const host_ui::PowerManagementModel& model);
    void LeavePowerManagement();
    [[nodiscard]] bool PowerManagementVisible() const;
    [[nodiscard]] void* PowerManagementActionContext() const;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppearanceLocked(
        lv_obj_t* root, const host_ui::AppearanceModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context);
    void UpdateAppearanceLocked(const host_ui::AppearanceModel& model);
    void LeaveAppearance();
    [[nodiscard]] bool AppearanceVisible() const;
    [[nodiscard]] void* AppearanceActionContext() const;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowRemoteControlLocked(
        lv_obj_t* root, const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context);
    void UpdateRemoteControlLocked(const host_ui::RemoteControlModel& model);
    void LeaveRemoteControl();
    [[nodiscard]] bool RemoteControlVisible() const;
    [[nodiscard]] void* RemoteControlActionContext() const;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppManagementLocked(
        lv_obj_t* root, const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context);
    void LeaveAppManagement();
    [[nodiscard]] bool AppManagementVisible() const;
    [[nodiscard]] void* AppManagementActionContext() const;

   private:
    enum class AppOverlay : uint8_t {
        kNone,
        kActions,
        kInformation,
        kUninstallConfirmation,
        kUninstallUnavailable,
    };

    enum class Screen : uint8_t {
        kNone,
        kSystemInformation,
        kPowerManagement,
        kAppearance,
        kRemoteControl,
        kAppManagement,
    };

    struct AppBinding final {
        SystemDetailUi* ui{};
        uint32_t index{};
    };

    static void ScrollEvent(lv_event_t* event);
    static void SystemInformationBackEvent(lv_event_t* event);
    static void SystemInformationUpdateEvent(lv_event_t* event);
    static void PowerManagementBackEvent(lv_event_t* event);
    static void PowerManagementSwitchEvent(lv_event_t* event);
    static void PowerManagementTimeoutEvent(lv_event_t* event);
    static void AppearanceBackEvent(lv_event_t* event);
    static void AppearanceThemeEvent(lv_event_t* event);
    static void RemoteControlBackEvent(lv_event_t* event);
    static void RemoteControlToggleEvent(lv_event_t* event);
    static void RemoteControlPairingEvent(lv_event_t* event);
    static void RemoteControlConfirmationCancelEvent(lv_event_t* event);
    static void RemoteControlConfirmOffEvent(lv_event_t* event);
    static void RemoteControlRenderAsync(void* context);
    static void AppManagementRenderAsync(void* context);
    static void AppManagementBackEvent(lv_event_t* event);
    static void AppManagementRowEvent(lv_event_t* event);
    static void AppManagementCancelEvent(lv_event_t* event);
    static void AppManagementOpenEvent(lv_event_t* event);
    static void AppManagementInformationEvent(lv_event_t* event);
    static void AppManagementUninstallEvent(lv_event_t* event);
    static void AppManagementConfirmUninstallEvent(lv_event_t* event);
    static void AppManagementDisplayEvent(lv_event_t* event);

    void RenderSystemInformationLocked();
    void RenderFirmwareUpdateLocked();
    void RenderPowerManagementLocked();
    void RenderAppearanceLocked();
    void RenderRemoteControlLocked();
    void DrawRemoteControlOffConfirmationLocked();
    void QueueRemoteControlRender();
    void RenderAppManagementLocked();
    void RenderAppManagementOverlayLocked();
    void DrawAppManagementActionsLocked();
    void DrawAppManagementInformationLocked();
    void DrawAppManagementUninstallUnavailableLocked();
    void DrawAppManagementUninstallConfirmationLocked();
    void QueueAppManagementRender();
    void BeginAppManagementLatencyProbe(const char* operation);
    void StartAppManagementLatencyProbe();
    void ResetActiveScreen();

    SystemPageLayout layout_{};
    lv_obj_t* root_{};
    host_ui::SystemInformationModel system_information_model_{};
    host_ui::PowerManagementModel power_management_model_{};
    host_ui::AppearanceModel appearance_model_{};
    host_ui::RemoteControlModel remote_control_model_{};
    host_ui::AppManagementModel app_management_model_{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
    lv_obj_t* power_switch_{};
    lv_obj_t* power_timeout_{};
    std::array<lv_obj_t*, 3> appearance_options_{};
    lv_obj_t* remote_control_scroll_{};
    int32_t remote_control_scroll_offset_{};
    platform::lvgl::AnimatedDisplayRefresh power_animation_refresh_{};
    std::array<AppBinding, host_ui::kMaxHallApps> app_bindings_{};
    uint32_t app_management_selected_index_{};
    AppOverlay app_management_overlay_{AppOverlay::kNone};
    lv_obj_t* app_management_overlay_root_{};
    lv_display_t* app_management_probe_display_{};
    const char* app_management_probe_operation_{};
    int64_t app_management_probe_touch_us_{};
    int64_t app_management_probe_render_start_us_{};
    int64_t app_management_probe_render_ready_us_{};
    bool app_management_probe_armed_{};
    bool remote_control_off_confirmation_visible_{};
    bool remote_control_scroll_gesture_active_{};
    bool remote_control_render_pending_{};
    bool updating_{};
    Screen active_screen_{Screen::kNone};
};

}  // namespace micropixel::host_ui::lvgl::square_common
