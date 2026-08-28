#pragma once

#include "lvgl.h"
#include "platform/lvgl/fonts/font_registry.hpp"

namespace micropixel::platform::lvgl::square_common {

// Creates the shared Host keyboard with LVGL's default maps and behavior.
// Callers own layout and screen-specific READY/CANCEL handling.
lv_obj_t* CreateDefaultKeyboard(lv_obj_t* parent, lv_obj_t* textarea, SystemFontRole font);

// Disables editing while keeping the default keyboard's close/hide action
// available so an in-progress operation can still be dismissed.
void SetDefaultKeyboardEditingEnabled(lv_obj_t* keyboard, bool enabled);

}  // namespace micropixel::platform::lvgl::square_common
