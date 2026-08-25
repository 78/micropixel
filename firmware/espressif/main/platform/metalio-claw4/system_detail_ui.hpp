#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_SYSTEM_DETAIL_UI_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_SYSTEM_DETAIL_UI_HPP

#include <array>
#include <cstdint>
#include <expected>

#include "host_ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::platform::metalio_claw4 {

// Owns the LVGL object references and interaction state for the detail
// screens reached from System Settings. The platform remains responsible for
// display locking and for binding the shared Host pointer input device.
class SystemDetailUi final {
   public:
    SystemDetailUi() = default;
    SystemDetailUi(const SystemDetailUi&) = delete;
    SystemDetailUi& operator=(const SystemDetailUi&) = delete;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformationLocked(
        lv_obj_t* root, const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink,
        void* action_context);
    void UpdateSystemInformationLocked(const host_ui::SystemInformationModel& model);
    void LeaveSystemInformation();
    [[nodiscard]] bool SystemInformationVisible() const;
    [[nodiscard]] void* SystemInformationActionContext() const;

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
        kRemoteControl,
        kAppManagement,
    };

    static void DetailScrollEvent(lv_event_t* event);
    static void SystemInformationBackEvent(lv_event_t* event);
    static void SystemInformationUpdateEvent(lv_event_t* event);
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

    void QueueAppManagementRender();
    void QueueRemoteControlRender();
    void RenderRemoteControlLocked();
    void DrawRemoteControlOffConfirmationLocked();
    void RenderAppManagementLocked(bool clean_root);
    void DrawAppManagementActionsLocked();
    void DrawAppManagementInformationLocked();
    void DrawAppManagementUninstallUnavailableLocked();
    void DrawAppManagementUninstallConfirmationLocked();
    void RefreshAppManagementRowsLocked();
    void DrawAppManagementRowLocked(uint32_t index);
    [[nodiscard]] uint32_t FindAppManagementRowIndex(lv_obj_t* row) const;

    lv_obj_t* root_{};
    lv_obj_t* app_management_scroll_{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> app_management_rows_{};
    host_ui::SystemUiActionSink system_information_action_sink_{};
    void* system_information_action_context_{};
    host_ui::SystemUiActionSink remote_control_action_sink_{};
    void* remote_control_action_context_{};
    host_ui::RemoteControlModel remote_control_model_{};
    bool remote_control_off_confirmation_visible_{};
    host_ui::SystemUiActionSink app_management_action_sink_{};
    void* app_management_action_context_{};
    host_ui::AppManagementModel app_management_model_{};
    uint32_t app_management_selected_index_{};
    int32_t app_management_scroll_y_{};
    AppOverlay app_management_overlay_{AppOverlay::kNone};
    Screen active_screen_{Screen::kNone};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
