#pragma once

#include <array>
#include <cstdint>

#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {

struct HallSceneRect final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

struct HallScenePoint final {
    int32_t x{};
    int32_t y{};
};

struct HallSceneSize final {
    int32_t width{};
    int32_t height{};
};

struct HallStatusBarLayout final {
    int32_t height{};
    int32_t padding_left{};
    int32_t padding_right{};
    int32_t item_gap{};
    int32_t battery_gap{};
    int32_t time_width{};
    HallSceneSize cellular{};
    uint16_t wifi_scale{256U};
    int32_t battery_width{};
    int32_t battery_percent_width{};
};

struct HallSceneLayout final {
    int32_t width{};
    int32_t height{};
    HallSceneRect brand{};
    int32_t brand_radius{};
    HallScenePoint title{};
    HallScenePoint section{};
    HallSceneRect settings_button{};
    HallSceneRect update_button{};
    int32_t header_button_radius{};
    int32_t header_button_icon_x{};
    int32_t header_button_label_x{};
    platform::lvgl::SystemFontRole header_button_font{platform::lvgl::SystemFontRole::kLarge};
    int32_t header_button_pad_horizontal{};
    HallSceneRect carousel{};
    int32_t card_width{};
    int32_t card_gap{};
    int32_t content_trailing_width{};
    uint32_t fully_visible_cards{3U};
    HallSceneRect scroll_track{};
    int32_t scroll_min_thumb_width{};
    HallScenePoint empty_message{};
    HallScenePoint failure_status{};
    HallScenePoint failure_app_id{};
    HallScenePoint failure_identity{};
    HallScenePoint failure_detail{};
    HallScenePoint simple_status{};
    HallScenePoint simple_app_id{};
    int32_t status_text_width{};
    HallStatusBarLayout status_bar{};
};

struct HallSceneEvents final {
    lv_event_cb_t carousel_event{};
    void* carousel_context{};
    host_ui::SystemUiActionSink action_sink{};
    void* action_context{};
};

[[nodiscard]] bool HallStatusBarMatches(const host_ui::HallStatusBarModel& left,
                                        const host_ui::HallStatusBarModel& right);

struct HallSceneObjects final {
    lv_obj_t* carousel_viewport{};
    lv_obj_t* carousel_content{};
    lv_obj_t* scroll_track{};
    lv_obj_t* scroll_thumb{};
    lv_obj_t* status_bar_container{};
    lv_obj_t* status_bar_items{};
    lv_obj_t* time_label{};
    lv_obj_t* cellular_container{};
    std::array<lv_obj_t*, 4U> cellular_bars{};
    lv_obj_t* wifi_image{};
    lv_obj_t* battery_container{};
    lv_obj_t* battery_label{};
    lv_obj_t* battery_percent_label{};
};

class HallSceneUi final {
   public:
    HallSceneUi() = default;
    HallSceneUi(const HallSceneUi&) = delete;
    HallSceneUi& operator=(const HallSceneUi&) = delete;

    void DrawLocked(lv_obj_t* root, const HallSceneLayout& layout, const host_ui::HallModel& model,
                    uint32_t visible_count, HallSceneEvents events);
    void ResetLocked();
    void UpdateStatusBarLocked(const host_ui::HallStatusBarModel& model);
    void UpdateScrollLocked(uint32_t app_count, int32_t offset);

    [[nodiscard]] const HallSceneObjects& objects() const { return objects_; }

   private:
    static void HeaderButtonEvent(lv_event_t* event);
    void HandleHeaderButtonEvent(lv_event_t* event);

    const HallSceneLayout* layout_{};
    HallSceneEvents events_{};
    HallSceneObjects objects_{};
    lv_obj_t* settings_button_{};
    lv_obj_t* update_button_{};
};

}  // namespace micropixel::host_ui::lvgl::square_common
