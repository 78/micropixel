#pragma once

#include <array>
#include <cstdint>
#include <expected>

#include "freertos/FreeRTOS.h"
#include "host/ui/lvgl/square_common/system_page_layout.hpp"
#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// Responsive Wi-Fi presentation shared by square displays. Network state and
// mutations remain owned by HostController; this class only renders a model
// and emits typed SystemUiAction values.
class WifiSettingsUi final {
   public:
    using RaiseOverlaySink = void (*)(void* context);

    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(
        lv_obj_t* root, lv_display_t* display, const SystemPageLayout& layout, const host_ui::WifiSettingsModel& model,
        host_ui::SystemUiActionSink action_sink, void* action_context, RaiseOverlaySink raise_overlay_sink,
        void* raise_overlay_context);
    void Update(const host_ui::WifiSettingsModel& model, bool pointer_busy);
    void Leave();
    void PointerReleased();
    [[nodiscard]] bool Visible() const { return visible_; }
    [[nodiscard]] void* ActionContext() const { return action_context_; }

   private:
    struct NetworkBinding final {
        WifiSettingsUi* ui{};
        uint32_t index{};
        bool saved{};
    };

    static void BackEvent(lv_event_t* event);
    static void SwitchEvent(lv_event_t* event);
    static void OpenScanEvent(lv_event_t* event);
    static void NetworkEvent(lv_event_t* event);
    static void SheetConnectEvent(lv_event_t* event);
    static void SheetForgetEvent(lv_event_t* event);
    static void OverlayCancelEvent(lv_event_t* event);
    static void PasswordConnectEvent(lv_event_t* event);
    static void PasswordKeyboardCancelEvent(lv_event_t* event);
    static void PasswordKeyboardReadyEvent(lv_event_t* event);
    static void ScrollEvent(lv_event_t* event);
    static void RenderAsync(void* context);

    void QueueRender();
    void RenderLocked();
    void DrawNetworkRow(lv_obj_t* parent, const host_ui::WifiNetworkModel& network, NetworkBinding& binding);
    void DrawActionSheetLocked();
    void DrawPasswordLocked();
    void EmitNetworkAction(host_ui::SystemUiActionType type, const host_ui::WifiNetworkModel& network,
                           const char* password = nullptr);

    const SystemPageLayout* layout_{};
    lv_obj_t* root_{};
    lv_display_t* display_{};
    lv_obj_t* scroll_content_{};
    lv_obj_t* password_textarea_{};
    lv_obj_t* keyboard_{};
    host_ui::WifiSettingsModel model_{};
    host_ui::WifiNetworkModel password_network_{};
    host_ui::WifiConnectionState password_connection_state_{host_ui::WifiConnectionState::kDisconnected};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
    RaiseOverlaySink raise_overlay_sink_{};
    void* raise_overlay_context_{};
    std::array<NetworkBinding, host_ui::kMaxSavedWifiNetworks + host_ui::kMaxVisibleWifiNetworks> bindings_{};
    std::array<char, host_ui::kMaxWifiPasswordLength + 1U> password_{};
    uint32_t selected_saved_index_{};
    int32_t scroll_offset_{};
    portMUX_TYPE render_lock_ = portMUX_INITIALIZER_UNLOCKED;
    bool scan_view_{};
    bool action_sheet_visible_{};
    bool password_visible_{};
    bool password_attempt_active_{};
    bool password_length_invalid_{};
    bool render_pending_{};
    bool user_scrolled_{};
    bool scroll_gesture_active_{};
    bool visible_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
