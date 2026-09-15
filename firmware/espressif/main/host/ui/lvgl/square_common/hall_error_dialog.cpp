// SPDX-License-Identifier: Apache-2.0
#include "host/ui/lvgl/square_common/hall_error_dialog.hpp"

#include <algorithm>

#include "host/ui/lvgl/square_common/host_ui_theme.hpp"

namespace micropixel::host_ui::lvgl::square_common {
namespace {
void Plain(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void Label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
    if (text == nullptr || text[0] == '\0') return;
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}
}  // namespace

HallErrorDialogObjects DrawHallErrorDialog(lv_obj_t* root, int32_t width, int32_t height,
                                           const HallErrorDialogText& text, const lv_font_t* title_font,
                                           const lv_font_t* body_font, lv_event_cb_t close_event, void* context) {
    const int32_t margin = width <= 320 ? 12 : 24;
    const int32_t padding = width <= 320 ? 12 : 20;
    const int32_t gap = width <= 320 ? 8 : 12;
    HallErrorDialogObjects objects;
    objects.overlay = lv_obj_create(root);
    Plain(objects.overlay);
    lv_obj_set_size(objects.overlay, width, height);
    lv_obj_add_flag(objects.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(objects.overlay, lv_color_hex(theme::kModalScrim), 0);
    lv_obj_set_style_bg_opa(objects.overlay, LV_OPA_70, 0);

    objects.panel = lv_obj_create(objects.overlay);
    Plain(objects.panel);
    lv_obj_set_size(objects.panel, width - 2 * margin, std::min<int32_t>(height - 2 * margin, 320));
    lv_obj_center(objects.panel);
    lv_obj_set_style_bg_color(objects.panel, lv_color_hex(theme::kPanelBackground), 0);
    lv_obj_set_style_bg_opa(objects.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(objects.panel, padding, 0);
    lv_obj_set_style_border_width(objects.panel, 1, 0);
    lv_obj_set_style_border_color(objects.panel, lv_color_hex(theme::kStrongBorder), 0);
    lv_obj_set_style_pad_all(objects.panel, padding, 0);
    lv_obj_set_style_pad_row(objects.panel, gap, 0);
    lv_obj_set_flex_flow(objects.panel, LV_FLEX_FLOW_COLUMN);
    Label(objects.panel, text.title, title_font, theme::kHallError);

    objects.body = lv_obj_create(objects.panel);
    Plain(objects.body);
    lv_obj_set_size(objects.body, LV_PCT(100), 0);
    lv_obj_set_flex_grow(objects.body, 1);
    lv_obj_add_flag(objects.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(objects.body, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(objects.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(objects.body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_row(objects.body, gap, 0);
    lv_obj_set_flex_flow(objects.body, LV_FLEX_FLOW_COLUMN);
    Label(objects.body, text.app_id, body_font, theme::kSecondaryText);
    Label(objects.body, text.identity, body_font, theme::kHallErrorDetail);
    Label(objects.body, text.detail, body_font, theme::kPrimaryText);

    objects.close = lv_button_create(objects.panel);
    Plain(objects.close);
    lv_obj_set_size(objects.close, LV_PCT(100), width <= 320 ? 36 : 48);
    lv_obj_set_style_bg_color(objects.close, lv_color_hex(theme::kActionBackground), 0);
    lv_obj_set_style_bg_opa(objects.close, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(objects.close, gap, 0);
    lv_obj_set_style_bg_color(objects.close, lv_color_hex(theme::kPressedBackground), LV_STATE_PRESSED);
    lv_obj_t* label = lv_label_create(objects.close);
    lv_label_set_text(label, text.close);
    lv_obj_set_style_text_font(label, body_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(theme::kPrimaryText), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(objects.close, close_event, LV_EVENT_SHORT_CLICKED, context);
    return objects;
}
}  // namespace micropixel::host_ui::lvgl::square_common
