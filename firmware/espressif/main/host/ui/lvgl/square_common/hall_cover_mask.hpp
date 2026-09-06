#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common {

// Only the top corners are masked; the bottom edge retains the cover pixels.
// The caller supplies the actual pixel stride and at least stride * size bytes.
// PPA transition covers are packed; LVGL cached covers may have row padding.
void MaskHallCoverRgb888(uint8_t* destination, uint32_t size, uint32_t stride, uint32_t corner_radius,
                         uint32_t top_background_rgb);

}  // namespace micropixel::host_ui::lvgl::square_common
