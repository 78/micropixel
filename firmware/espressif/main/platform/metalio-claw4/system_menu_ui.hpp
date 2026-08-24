#pragma once

#include <cstdint>
#include <expected>

#include "device/input.hpp"
#include "host_ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::platform::metalio_claw4 {

class SystemMenuUi final {
   public:
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                         const host_ui::SystemMenuModel& model,
                                                                         host_ui::SystemUiActionSink action_sink,
                                                                         void* action_context);
    void Deactivate();

    [[nodiscard]] void* ActionContext() const { return action_context_; }
    [[nodiscard]] bool Active() const { return action_sink_ != nullptr || action_context_ != nullptr; }
    static bool TouchSink(void* context, const device::TouchSample& sample);

   private:
    friend struct SystemMenuUiAccess;

    enum class TouchTarget : uint8_t {
        kNone,
        kBack,
        kWifi,
        kLanguage,
        kSystemInformation,
        kManageApps,
    };

    [[nodiscard]] bool HandleTouch(const device::TouchSample& sample);
    void SetTargetPressed(TouchTarget target, bool pressed);
    void ResetObjectPointers();

    lv_display_t* display_{};
    lv_obj_t* press_overlays_[5]{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
    uint32_t touch_id_{};
    TouchTarget touch_target_{TouchTarget::kNone};
    int32_t touch_down_x_{};
    int32_t touch_down_y_{};
    uint64_t touch_down_us_{};
    bool touch_active_{};
    bool button_pressed_{};
};

}  // namespace micropixel::platform::metalio_claw4
