#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "host_ui/system_ui.hpp"
#include "lvgl.h"

namespace micropixel::platform::lvgl::square_720 {

class SystemMenuUi final {
   public:
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                         const host_ui::SystemMenuModel& model,
                                                                         host_ui::SystemUiActionSink action_sink,
                                                                         void* action_context);
    void Update(const host_ui::SystemMenuModel& model);
    void Deactivate();

    [[nodiscard]] void* ActionContext() const { return action_context_; }
    [[nodiscard]] bool Active() const { return action_sink_ != nullptr || action_context_ != nullptr; }

   private:
    struct RowBinding final {
        SystemMenuUi* ui{};
        host_ui::SystemMenuItem item{host_ui::SystemMenuItem::kWifi};
    };

    void DrawRow(lv_obj_t* parent, size_t index, host_ui::SystemMenuItem item, const char* icon_text, const char* name,
                 const char* detail, uint32_t icon_color, bool emphasized_border = false);
    void ResetObjectPointers();
    static void BackEvent(lv_event_t* event);
    static void RowEvent(lv_event_t* event);
    static void ScrollEvent(lv_event_t* event);

    lv_display_t* display_{};
    lv_obj_t* scroll_content_{};
    lv_obj_t* wifi_detail_label_{};
    lv_obj_t* remote_control_detail_label_{};
    lv_obj_t* system_information_detail_label_{};
    lv_obj_t* power_management_detail_label_{};
    lv_obj_t* firmware_update_dot_{};
    std::array<RowBinding, 6> row_bindings_{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
};

}  // namespace micropixel::platform::lvgl::square_720
