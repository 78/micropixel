#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "device/contracts/graphics.hpp"
#include "host/ui/lvgl/square_common/guest_gesture_hint_ui.hpp"
#include "host/ui/lvgl/square_common/hall_card_ui.hpp"
#include "host/ui/lvgl/square_common/hall_cover_cache.hpp"
#include "host/ui/lvgl/square_common/hall_scene_ui.hpp"
#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "host/ui/lvgl/square_common/layout.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "host/ui/lvgl/square_common/status_layer_ui.hpp"
#include "host/ui/lvgl/square_common/system_detail_ui.hpp"
#include "host/ui/lvgl/square_common/system_menu_ui.hpp"
#include "host/ui/lvgl/square_common/wifi_settings_ui.hpp"
#include "host/ui/system_gesture_router.hpp"
#include "lvgl.h"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/host_pointer_router.hpp"

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::host_ui::lvgl::square_common {

inline constexpr size_t kHallAppTextCapacity = 65U;
inline constexpr uint32_t kHostPointerQueueCapacity = 32U;

struct SquareSystemUiProfile final {
    const SquareLayout& square;
    const HallCardLayout& hall_card;
    const HallSceneLayout& hall_scene;
    const SystemMenuLayout& system_menu;
    const SystemPageLayout& system_page;
    int32_t launch_label_bottom_offset{};
    bool scale_oversized_launch_bitmap{};
    bool derive_launch_background{};
    bool allow_software_status_animation{};
};

struct HallAppPresentation final {
    std::array<char, kHallAppTextCapacity> app_id{};
    std::array<char, kHallAppTextCapacity> display_name{};
    uint8_t install_progress_percent{};
    bool running{};
    bool installing{};
};

// Host-owned LVGL state shared by every square-display product. Board state
// owns one instance but does not duplicate page objects, input routing, Hall
// bookkeeping or System UI lifecycle rules.
class SquareSystemUiState final {
   public:
    using ResetHallCallback = void (*)(void* context);
    using ThemeChangedCallback = void (*)(void* context);
    using PrepareHardwareCallback = void (*)(void* context);

    SquareSystemUiState(device::Input& physical_input, platform::lvgl::GuestGraphicsEngine& guest_graphics,
                        StatusLayerTransition& transition, const SquareSystemUiProfile& profile);
    SquareSystemUiState(const SquareSystemUiState&) = delete;
    SquareSystemUiState& operator=(const SquareSystemUiState&) = delete;

    void BindHallReset(ResetHallCallback reset, void* context);
    void BindThemeChanged(ThemeChangedCallback changed, void* context);
    void BindBeforeLaunchPresentation(PrepareHardwareCallback prepare_locked, void* context);
    void BindBeforeRootRelease(PrepareHardwareCallback prepare_locked, void* context);
    void BindBackgroundExecutor(work::BackgroundExecutor& executor);
    [[nodiscard]] esp_err_t InitializeLocked(lv_display_t* display);
    [[nodiscard]] platform::lvgl::GuestGraphicsHooks GraphicsHooks();
    [[nodiscard]] platform::lvgl::GuestPresentationHooks GuestFrameHooks();
    [[nodiscard]] device::Input& Input() { return input_router; }
    [[nodiscard]] lv_obj_t* GuestFrameLocked() const { return guest_graphics_.FrameLocked(); }
    [[nodiscard]] bool GuestRefreshSynchronizationAvailable() const {
        return guest_graphics_.RefreshSynchronizationAvailable();
    }
    void DrainGuestRefreshReady() { guest_graphics_.DrainRefreshReady(); }
    void WaitForGuestRefreshReady() { guest_graphics_.WaitForRefreshReady(); }

    [[nodiscard]] lv_obj_t* EnsureRootLocked(uint32_t background);
    [[nodiscard]] lv_obj_t* PrepareSystemPageRootLocked();
    void DeleteRootLocked();
    void ResetHallPresentationLocked();
    [[nodiscard]] int32_t ShowLaunchBitmap(const device::BitmapView& bitmap);
    [[nodiscard]] bool DismissLaunchBitmap();
    void SetHostPointerEnabledLocked(bool enabled);
    [[nodiscard]] bool HostPointerBusy();
    void BindHostPointerTouchSink();
    void UnbindHostPointerTouchSink();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemMenu(const host_ui::SystemMenuModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context);
    void UpdateSystemMenu(const host_ui::SystemMenuModel& model);
    void LeaveSystemMenu();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowSystemInformation(
        const host_ui::SystemInformationModel& model, host_ui::SystemUiActionSink action_sink, void* action_context);
    void UpdateSystemInformation(const host_ui::SystemInformationModel& model);
    void LeaveSystemInformation();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowPowerManagement(
        const host_ui::PowerManagementModel& model, host_ui::SystemUiActionSink action_sink, void* action_context);
    void UpdatePowerManagement(const host_ui::PowerManagementModel& model);
    void LeavePowerManagement();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppearance(const host_ui::AppearanceModel& model,
                                                                             host_ui::SystemUiActionSink action_sink,
                                                                             void* action_context);
    void UpdateAppearance(const host_ui::AppearanceModel& model);
    void LeaveAppearance();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowRemoteControl(
        const host_ui::RemoteControlModel& model, host_ui::SystemUiActionSink action_sink, void* action_context);
    void UpdateRemoteControl(const host_ui::RemoteControlModel& model);
    void LeaveRemoteControl();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowAppManagement(
        const host_ui::AppManagementModel& model, host_ui::SystemUiActionSink action_sink, void* action_context);
    void LeaveAppManagement();

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowWifiSettings(const host_ui::WifiSettingsModel& model,
                                                                               host_ui::SystemUiActionSink action_sink,
                                                                               void* action_context);
    void UpdateWifiSettings(const host_ui::WifiSettingsModel& model);
    void LeaveWifiSettings();

    void WatchGuestActions(host_ui::SystemUiActionSink action_sink, void* action_context);
    void StopWatchingGuestActions(void* action_context);
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowStatusLayer(const host_ui::StatusLayerModel& model,
                                                                              uint64_t trigger_timestamp_us,
                                                                              host_ui::SystemUiActionSink action_sink,
                                                                              void* action_context);
    void UpdateStatusLayer(const host_ui::StatusLayerModel& model);
    void LeaveStatusLayer(uint64_t trigger_timestamp_us);
    void UpdatePerformanceOverlay(bool enabled, uint8_t cpu_percent);
    void ApplyTheme(host_ui::SystemThemeMode mode);
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowShutdown(
        PrepareHardwareCallback prepare_locked = nullptr, void* prepare_context = nullptr);

    const SquareSystemUiProfile& profile;
    lv_display_t* display{};
    lv_obj_t* root{};
    lv_image_dsc_t launch_image_descriptor{};
    GuestGestureHintUi guest_gesture_hint_ui{};
    StatusLayerUi status_layer_ui{};
    HallSceneUi hall_scene_ui{};
    SystemMenuUi system_menu_ui{};
    SystemDetailUi system_detail_ui;
    WifiSettingsUi wifi_settings_ui{};
    platform::lvgl::HostPointerRouter<kHostPointerQueueCapacity> host_pointer{};
    host_ui::SystemGestureRouter input_router;
    HallCoverCache hall_cover_cache;

    std::array<lv_image_dsc_t, host_ui::kMaxHallApps> hall_cover_descriptors{};
    std::array<host_ui::HallCoverModel, host_ui::kMaxHallApps> hall_cover_sources{};
    std::array<host_ui::HallCoverModel, host_ui::kMaxHallApps> hall_idle_cover_sources{};
    std::array<HallAppPresentation, host_ui::kMaxHallApps> hall_app_presentations{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> hall_cards{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> hall_cover_images{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> hall_cover_placeholders{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> hall_install_progress_arcs{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> hall_install_progress_labels{};
    std::array<lv_obj_t*, host_ui::kMaxHallApps> hall_card_press_overlays{};
    std::array<bool, host_ui::kMaxHallApps> hall_app_running{};
    host_ui::SystemUiActionSink hall_action_sink{};
    void* hall_action_context{};
    uint32_t hall_app_count{};
    uint32_t hall_card_window_first{host_ui::kMaxHallApps};
    uint32_t hall_card_window_last{host_ui::kMaxHallApps};
    int32_t hall_scroll_offset{};
    uint64_t hall_catalog_signature{};
    host_ui::HallStatusBarModel hall_status_bar{};
    bool hall_firmware_update_available{};
    bool hall_status_bar_valid{};
    bool hall_launch_enabled{};
    bool hall_retained_for_status{};

   private:
    static bool HostPointerTouchSink(void* context, const device::TouchSample& sample);
    static theme::Mode ThemeMode(host_ui::SystemThemeMode mode);
    void RefreshPerformanceOverlayLocked();
    void ShowStartingScreenLocked();
    void PrepareGuestFrameLocked(lv_obj_t* guest_frame, bool created_guest_frame, bool& needs_present);
    void ResetHallLocked();
    void BindPageInput(host_ui::SystemUiActionSink action_sink, void* action_context);
    void UnbindPageInput(void* action_context);

    platform::lvgl::GuestGraphicsEngine& guest_graphics_;
    StatusLayerTransition& transition_;
    ResetHallCallback reset_hall_locked_{};
    void* reset_hall_context_{};
    ThemeChangedCallback theme_changed_locked_{};
    void* theme_changed_context_{};
    PrepareHardwareCallback before_launch_presentation_locked_{};
    void* before_launch_presentation_context_{};
    PrepareHardwareCallback before_root_release_locked_{};
    void* before_root_release_context_{};
    uint8_t performance_cpu_percent_{};
    bool performance_overlay_requested_{};
    bool guest_actions_watched_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
