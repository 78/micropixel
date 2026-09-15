// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lvgl.h"

namespace micropixel::host_ui::lvgl::square_common {

struct HallErrorDialogText final {
    const char* title{};
    const char* app_id{};
    const char* identity{};
    const char* detail{};
    const char* close{};
};

struct HallErrorDialogObjects final {
    lv_obj_t* overlay{};
    lv_obj_t* panel{};
    lv_obj_t* body{};
    lv_obj_t* close{};
};

// The body scrolls independently; the close button always stays inside the display.
[[nodiscard]] HallErrorDialogObjects DrawHallErrorDialog(lv_obj_t* root, int32_t width, int32_t height,
                                                         const HallErrorDialogText& text, const lv_font_t* title_font,
                                                         const lv_font_t* body_font, lv_event_cb_t close_event,
                                                         void* context);

}  // namespace micropixel::host_ui::lvgl::square_common
