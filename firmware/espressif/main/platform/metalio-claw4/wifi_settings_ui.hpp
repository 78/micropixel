#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_WIFI_SETTINGS_UI_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_WIFI_SETTINGS_UI_HPP

#include <array>
#include <cstdint>
#include <expected>

#include "freertos/FreeRTOS.h"
#include "host_ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::platform::metalio_claw4 {

struct WifiSettingsUiAccess;

// Owns all LVGL objects, dialog state and deferred-render bookkeeping for the
// Wi-Fi settings screen. The platform supplies only the shared Host root, the
// input-pointer busy state and a callback that restores the final HUD z-order.
class WifiSettingsUi final {
   public:
    using RaiseOverlaySink = void (*)(void* context);

    WifiSettingsUi() = default;
    WifiSettingsUi(const WifiSettingsUi&) = delete;
    WifiSettingsUi& operator=(const WifiSettingsUi&) = delete;

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                         const host_ui::WifiSettingsModel& model,
                                                                         host_ui::SystemUiActionSink action_sink,
                                                                         void* action_context,
                                                                         RaiseOverlaySink raise_overlay_sink,
                                                                         void* raise_overlay_context);
    void Update(const host_ui::WifiSettingsModel& model, bool pointer_busy);
    void Leave();
    void PointerReleased();
    [[nodiscard]] bool Visible() const;
    [[nodiscard]] void* ActionContext() const;

   private:
    friend struct WifiSettingsUiAccess;

    lv_obj_t* root{};
    lv_display_t* display{};
    lv_obj_t* wifi_scroll_content{};
    lv_obj_t* wifi_password_textarea{};
    lv_obj_t* wifi_keyboard{};
    lv_obj_t* wifi_saved_rows[host_ui::kMaxSavedWifiNetworks]{};
    lv_obj_t* wifi_available_rows[host_ui::kMaxVisibleWifiNetworks]{};
    host_ui::SystemUiActionSink wifi_action_sink{};
    void* wifi_action_context{};
    RaiseOverlaySink raise_overlay_sink{};
    void* raise_overlay_context{};
    host_ui::WifiSettingsModel wifi_model{};
    host_ui::WifiNetworkModel wifi_password_network{};
    host_ui::WifiConnectionState wifi_password_connection_state{host_ui::WifiConnectionState::kDisconnected};
    uint32_t wifi_selected_index{};
    int32_t wifi_scroll_offset{};
    std::array<char, host_ui::kMaxWifiPasswordLength + 1U> wifi_password{};
    portMUX_TYPE render_lock = portMUX_INITIALIZER_UNLOCKED;
    bool wifi_action_sheet_visible{};
    bool wifi_password_visible{};
    bool wifi_password_attempt_active{};
    bool wifi_render_pending{};
    bool wifi_user_scrolled{};
    bool wifi_scroll_gesture_active{};
    bool visible{};
};

}  // namespace micropixel::platform::metalio_claw4

#endif
