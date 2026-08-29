#pragma once

#include <cstdint>

#include "lvgl.h"

namespace micropixel::platform::lvgl {

struct DirtyRegionStats final {
    uint32_t input_regions{};
    uint32_t output_regions{};
    uint64_t input_pixels{};
    uint64_t output_pixels{};
};

// Reduces LVGL invalid-area fragmentation before a display refresh.
class DirtyRegionCoalescer final {
   public:
    [[nodiscard]] DirtyRegionStats Coalesce(lv_display_t* display);
};

}  // namespace micropixel::platform::lvgl
