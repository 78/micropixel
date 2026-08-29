#include "host/ui/lvgl/square_common/default_keyboard.hpp"

#include <algorithm>
#include <cstring>

#include "host/ui/lvgl/square_common/host_ui_theme.hpp"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

namespace micropixel::host_ui::lvgl::square_common {
namespace {

void ConfigureDefaultKeyboardButtons(lv_obj_t* keyboard) {
    constexpr auto kReleaseWithoutRepeat =
        static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    lv_buttonmatrix_set_button_ctrl_all(keyboard, kReleaseWithoutRepeat);
}

uint32_t DefaultKeyboardButtonAtPoint(lv_obj_t* keyboard, const lv_point_t& point) {
    auto* matrix = reinterpret_cast<lv_buttonmatrix_t*>(keyboard);
    lv_area_t keyboard_area{};
    lv_obj_get_coords(keyboard, &keyboard_area);
    const int32_t width = lv_obj_get_width(keyboard);
    const int32_t height = lv_obj_get_height(keyboard);
    const int32_t left_padding = lv_obj_get_style_pad_left(keyboard, LV_PART_MAIN);
    const int32_t right_padding = lv_obj_get_style_pad_right(keyboard, LV_PART_MAIN);
    const int32_t top_padding = lv_obj_get_style_pad_top(keyboard, LV_PART_MAIN);
    const int32_t bottom_padding = lv_obj_get_style_pad_bottom(keyboard, LV_PART_MAIN);
    constexpr int32_t kMaxExtra = LV_DPI_DEF / 10;
    const int32_t row_padding = lv_obj_get_style_pad_row(keyboard, LV_PART_MAIN);
    const int32_t column_padding = lv_obj_get_style_pad_column(keyboard, LV_PART_MAIN);
    const int32_t row_extra = std::min((row_padding / 2) + 1 + (row_padding & 1), kMaxExtra);
    const int32_t column_extra = std::min((column_padding / 2) + 1 + (column_padding & 1), kMaxExtra);

    for (uint32_t button = 0U; button < matrix->btn_cnt; ++button) {
        lv_area_t area = matrix->button_areas[button];
        area.x1 += keyboard_area.x1 - (area.x1 <= left_padding ? std::min(left_padding, kMaxExtra) : column_extra);
        area.y1 += keyboard_area.y1 - (area.y1 <= top_padding ? std::min(top_padding, kMaxExtra) : row_extra);
        area.x2 += keyboard_area.x1 +
                   (area.x2 >= width - right_padding - 2 ? std::min(right_padding, kMaxExtra) : column_extra);
        area.y2 += keyboard_area.y1 +
                   (area.y2 >= height - bottom_padding - 2 ? std::min(bottom_padding, kMaxExtra) : row_extra);
        if (point.x >= area.x1 && point.x <= area.x2 && point.y >= area.y1 && point.y <= area.y2) {
            return button;
        }
    }
    return LV_BUTTONMATRIX_BUTTON_NONE;
}

void DefaultKeyboardTrackPointerEvent(lv_event_t* event) {
    lv_obj_t* keyboard = lv_event_get_current_target_obj(event);
    lv_indev_t* indev = lv_event_get_indev(event);
    if (keyboard == nullptr || indev == nullptr) {
        return;
    }
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    lv_buttonmatrix_set_selected_button(keyboard, DefaultKeyboardButtonAtPoint(keyboard, point));
}

void DefaultKeyboardPreviewEvent(lv_event_t* event) {
    lv_obj_t* preview = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    if (preview == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_t* keyboard = lv_event_get_current_target_obj(event);
    if (keyboard == nullptr) {
        return;
    }
    const uint32_t button = lv_keyboard_get_selected_button(keyboard);
    const char* text = lv_keyboard_get_button_text(keyboard, button);
    lv_obj_t* label = lv_obj_get_child(preview, 0);
    if (text == nullptr || label == nullptr) {
        lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(label, std::strcmp(text, " ") == 0 ? "Space" : text);
    lv_obj_center(label);
    lv_obj_align_to(preview, keyboard, LV_ALIGN_OUT_TOP_MID, 0, -8);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview);
}

void DefaultKeyboardValueChangedEvent(lv_event_t* event) {
    lv_obj_t* keyboard = lv_event_get_current_target_obj(event);
    if (keyboard != nullptr) {
        ConfigureDefaultKeyboardButtons(keyboard);
    }
}

}  // namespace

lv_obj_t* CreateDefaultKeyboard(lv_obj_t* parent, lv_obj_t* textarea, platform::lvgl::SystemFontRole font) {
    lv_obj_t* keyboard = lv_keyboard_create(parent);
    lv_keyboard_set_textarea(keyboard, textarea);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(keyboard, false);
    lv_obj_set_style_text_font(keyboard, platform::lvgl::BuiltinLatinFont(font), LV_PART_ITEMS);
    ConfigureDefaultKeyboardButtons(keyboard);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardTrackPointerEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardValueChangedEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* preview = lv_obj_create(parent);
    lv_obj_set_size(preview, 120, 64);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_set_style_radius(preview, 18, 0);
    lv_obj_set_style_border_width(preview, 2, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(theme::kAccent), 0);
    lv_obj_set_style_bg_color(preview, lv_color_hex(theme::kActionBackground), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* preview_label = lv_label_create(preview);
    lv_obj_set_style_text_font(preview_label, platform::lvgl::BuiltinLatinFont(platform::lvgl::SystemFontRole::kTitle),
                               0);
    lv_obj_set_style_text_color(preview_label, lv_color_hex(theme::kPrimaryText), 0);
    lv_obj_center(preview_label);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardPreviewEvent, LV_EVENT_PRESSED, preview);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardPreviewEvent, LV_EVENT_PRESSING, preview);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardPreviewEvent, LV_EVENT_RELEASED, preview);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardPreviewEvent, LV_EVENT_PRESS_LOST, preview);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardPreviewEvent, LV_EVENT_CANCEL, preview);
    lv_obj_add_event_cb(keyboard, DefaultKeyboardPreviewEvent, LV_EVENT_READY, preview);
    return keyboard;
}

void SetDefaultKeyboardEditingEnabled(lv_obj_t* keyboard, bool enabled) {
    if (keyboard == nullptr) {
        return;
    }
    if (enabled) {
        lv_buttonmatrix_clear_button_ctrl_all(keyboard, LV_BUTTONMATRIX_CTRL_DISABLED);
        return;
    }

    lv_buttonmatrix_set_button_ctrl_all(keyboard, LV_BUTTONMATRIX_CTRL_DISABLED);
    const char* const* map = lv_keyboard_get_map_array(keyboard);
    uint32_t button = 0U;
    for (const char* text = map != nullptr ? *map : nullptr; text != nullptr && text[0] != '\0'; text = *++map) {
        if (std::strcmp(text, "\n") == 0) {
            continue;
        }
        if (std::strcmp(text, LV_SYMBOL_CLOSE) == 0 || std::strcmp(text, LV_SYMBOL_KEYBOARD) == 0) {
            lv_buttonmatrix_clear_button_ctrl(keyboard, button, LV_BUTTONMATRIX_CTRL_DISABLED);
        }
        ++button;
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
