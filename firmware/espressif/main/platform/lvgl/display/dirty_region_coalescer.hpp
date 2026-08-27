#pragma once

#include "lvgl.h"

namespace micropixel::platform::lvgl {

// Reduces LVGL invalid-area fragmentation before a display refresh.
class DirtyRegionCoalescer final {
   public:
    void Coalesce(lv_display_t* display);
};

}  // namespace micropixel::platform::lvgl
