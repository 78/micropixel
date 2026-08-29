#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {

// System Settings owns one responsive Flex presentation and interaction
// implementation. Board profiles provide density and safe-area tokens, never
// per-object coordinates.
struct SystemMenuLayout final {
    int32_t width{};
    int32_t height{};
    int32_t header_height{};
    int32_t header_padding_horizontal{};
    int32_t header_padding_top{};
    int32_t header_gap{};
    int32_t back_button_size{};
    int32_t back_button_radius{};
    int32_t back_button_hit_padding{};
    int32_t header_text_gap{};
    int32_t content_padding_horizontal{};
    int32_t content_padding_top{};
    int32_t content_padding_bottom{};
    int32_t row_height{};
    int32_t row_gap{};
    int32_t row_radius{};
    int32_t row_padding_horizontal{};
    int32_t row_icon_width{};
    int32_t row_content_gap{};
    int32_t row_text_gap{};
    int32_t row_chevron_width{};
    int32_t firmware_dot_size{};
    int32_t scrollbar_width{};
    int32_t scrollbar_radius{};
    platform::lvgl::SystemFontRole row_icon_font{platform::lvgl::SystemFontRole::kLarge};
    platform::lvgl::SystemFontRole row_name_font{platform::lvgl::SystemFontRole::kLarge};
    platform::lvgl::SystemFontRole row_detail_font{platform::lvgl::SystemFontRole::kMedium};
};

class SystemMenuUi final {
   public:
    [[nodiscard]] std::expected<void, host_ui::SystemUiError> ShowLocked(lv_obj_t* root, lv_display_t* display,
                                                                         const SystemMenuLayout& layout,
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

    const SystemMenuLayout* layout_{};
    lv_display_t* display_{};
    lv_obj_t* scroll_content_{};
    lv_obj_t* wifi_detail_label_{};
    lv_obj_t* remote_control_detail_label_{};
    lv_obj_t* system_information_detail_label_{};
    lv_obj_t* appearance_detail_label_{};
    lv_obj_t* power_management_detail_label_{};
    lv_obj_t* firmware_update_dot_{};
    std::array<RowBinding, 7> row_bindings_{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
