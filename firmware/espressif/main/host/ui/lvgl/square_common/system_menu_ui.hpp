#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>

#include "host/ui/lvgl/square_common/action_sheet_presenter.hpp"
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
    SystemMenuUi(const SystemPageLayout& page_layout, ActionSheetPresenter& presenter)
        : page_layout_(page_layout), presenter_(presenter) {}
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
        uint32_t index{};
        host_ui::SystemMenuItem item{host_ui::SystemMenuItem::kWifi};
    };

    void DrawRow(lv_obj_t* parent, size_t index, host_ui::SystemMenuItem item, const char* icon_text, const char* name,
                 const char* detail, uint32_t icon_color, bool emphasized_border = false, bool interactive = true);
    bool UpdateLanguageSheetLocked(const host_ui::SystemMenuModel& model);
    void UpdateLanguageProgressLocked();
    void UpdateFontButtonLocked(const SystemMenuModel& model);
    void UpdateBadgesLocked(const SystemMenuModel& model);
    void ResetObjectPointers();
    static void UpdateFontEvent(lv_event_t* event);
    static void ConfirmLanguageEvent(lv_event_t* event);
    static void CancelLanguageEvent(lv_event_t* event);
    static void LanguageProgressTimer(lv_timer_t* timer);
    static void LanguageSheetDeleted(lv_event_t* event);
    static void BackEvent(lv_event_t* event);
    static void RowEvent(lv_event_t* event);
    static void ScrollEvent(lv_event_t* event);

    const SystemPageLayout& page_layout_;
    ActionSheetPresenter& presenter_;
    lv_obj_t* root_{};
    lv_obj_t* language_overlay_{};
    lv_obj_t* language_sheet_detail_{};
    lv_obj_t* language_version_label_{};
    lv_obj_t* language_size_label_{};
    lv_obj_t* language_space_label_{};
    lv_obj_t* language_progress_bar_{};
    lv_obj_t* language_confirm_{};
    lv_obj_t* language_cancel_{};
    lv_timer_t* language_timer_{};
    LanguageDownloadState language_state_{};
    LanguageDownloadState rendered_language_state_{};
    uint32_t rendered_download_bytes_{};
    uint32_t rendered_required_bytes_{};
    uint32_t rendered_free_bytes_{};
    uint8_t (*progress_reader_)(void*){};
    void* progress_context_{};
    const char* download_text_{};
    const char* install_text_{};
    uint8_t shown_progress_{255U};
    const SystemMenuLayout* layout_{};
    lv_display_t* display_{};
    lv_obj_t* scroll_content_{};
    lv_obj_t* language_status_label_{};
    bool language_view_{};
    lv_obj_t* wifi_detail_label_{};
    lv_obj_t* cellular_detail_label_{};
    lv_obj_t* remote_control_detail_label_{};
    lv_obj_t* system_information_detail_label_{};
    lv_obj_t* appearance_detail_label_{};
    lv_obj_t* power_management_detail_label_{};
    lv_obj_t* firmware_update_dot_{};
    lv_obj_t* language_update_dot_{};
    lv_obj_t* apps_update_dot_{};
    lv_obj_t* font_update_button_{};
    bool font_update_shown_{};
    std::array<RowBinding, 8> row_bindings_{};
    host_ui::SystemUiActionSink action_sink_{};
    void* action_context_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
