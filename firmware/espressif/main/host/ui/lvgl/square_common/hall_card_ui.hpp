#pragma once

#include <cstdint>

#include "host/ui/system_ui.hpp"
#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::host_ui::lvgl::square_common {

struct HallCardLayout final {
    int32_t width{};
    int32_t height{};
    int32_t radius{};
    int32_t border_width{};
    int32_t label_y{};
    int32_t label_x{};
    int32_t label_width{};
    int32_t label_height{};
    int32_t running_x{};
    int32_t running_y{};
    int32_t running_width{};
    int32_t running_height{};
    int32_t stop_x{};
    int32_t stop_y{};
    int32_t stop_size{};
    int32_t stop_hit_padding{};
    int32_t stop_icon_size{};
    platform::lvgl::SystemFontRole label_font{platform::lvgl::SystemFontRole::kSmall};
    platform::lvgl::SystemFontRole badge_font{platform::lvgl::SystemFontRole::kSmall};
};

struct HallCardPresentation final {
    const char* app_id{};
    const char* display_name{};
    uint8_t install_progress_percent{};
    bool running{};
    bool installing{};
};

struct HallCardObjects final {
    lv_obj_t* card{};
    lv_obj_t* cover_image{};
    lv_obj_t* cover_placeholder{};
    lv_obj_t* install_progress_arc{};
    lv_obj_t* install_progress_label{};
    lv_obj_t* press_overlay{};
};

void DrawHallCard(lv_obj_t* parent, const HallCardLayout& layout, const HallCardPresentation& app, uint32_t index,
                  lv_event_cb_t card_event, lv_event_cb_t stop_event, void* event_context, HallCardObjects& objects);
void SetHallCardPressed(const HallCardObjects& objects, bool pressed);
void SetHallCardInstallProgress(const HallCardObjects& objects, uint8_t progress_percent);
void ShowHallCoverPlaceholder(const HallCardObjects& objects);
void DetachHallCover(const HallCardObjects& objects, lv_image_dsc_t& descriptor);
void AttachHallCover(const HallCardLayout& layout, const HallCardObjects& objects, lv_image_dsc_t& descriptor,
                     const host_ui::HallCoverModel& cover);

}  // namespace micropixel::host_ui::lvgl::square_common
