#include "platform/metalio-claw4/display/dirty_region_coalescer.hpp"

#include <cstdint>

#include "sdkconfig.h"
#include "src/display/lv_display_private.h"

namespace micropixel::platform::metalio_claw4 {

void DirtyRegionCoalescer::Coalesce(lv_display_t* display) {
    bool merged = true;
    while (merged) {
        merged = false;
        for (uint32_t first = 0U; first < display->inv_p && !merged; ++first) {
            for (uint32_t second = first + 1U; second < display->inv_p; ++second) {
                const lv_area_t& a = display->inv_areas[first];
                const lv_area_t& b = display->inv_areas[second];
                lv_area_t joined{
                    .x1 = LV_MIN(a.x1, b.x1),
                    .y1 = LV_MIN(a.y1, b.y1),
                    .x2 = LV_MAX(a.x2, b.x2),
                    .y2 = LV_MAX(a.y2, b.y2),
                };
                const uint64_t source_pixels =
                    static_cast<uint64_t>(lv_area_get_size(&a)) + static_cast<uint64_t>(lv_area_get_size(&b));
                const uint64_t joined_pixels = static_cast<uint64_t>(lv_area_get_size(&joined));
                const uint64_t extra_pixels = joined_pixels > source_pixels ? joined_pixels - source_pixels : 0U;
                if (extra_pixels > CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_EXTRA_PIXELS ||
                    joined_pixels > CONFIG_MICROPIXEL_LVGL_DIRTY_COALESCE_MAX_PIXELS) {
                    continue;
                }
                display->inv_areas[first] = joined;
                display->inv_areas[second] = display->inv_areas[display->inv_p - 1U];
                --display->inv_p;
                merged = true;
                break;
            }
        }
    }
}

}  // namespace micropixel::platform::metalio_claw4
