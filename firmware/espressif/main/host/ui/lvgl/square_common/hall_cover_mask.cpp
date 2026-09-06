#include "host/ui/lvgl/square_common/hall_cover_mask.hpp"

#include <algorithm>
#include <cstddef>

namespace micropixel::host_ui::lvgl::square_common {

void MaskHallCoverRgb888(uint8_t* destination, uint32_t size, uint32_t stride, uint32_t radius,
                         uint32_t top_background_rgb, uint32_t bottom_background_rgb) {
    if (destination == nullptr || size == 0U || size > UINT32_MAX / 3U || stride < size * 3U || radius <= 1U) {
        return;
    }
    radius = std::min(radius, size / 2U);
    const int32_t center = static_cast<int32_t>(radius - 1U);
    const int32_t radius_squared = center * center;
    const auto set_pixel = [destination, stride](uint32_t x, uint32_t y, uint32_t rgb) {
        uint8_t* pixel = destination + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3U;
        pixel[0] = static_cast<uint8_t>(rgb);
        pixel[1] = static_cast<uint8_t>(rgb >> 8U);
        pixel[2] = static_cast<uint8_t>(rgb >> 16U);
    };
    for (uint32_t y = 0U; y < radius; ++y) {
        for (uint32_t x = 0U; x < radius; ++x) {
            const int32_t dx = static_cast<int32_t>(x) - center;
            const int32_t dy = static_cast<int32_t>(y) - center;
            if (dx * dx + dy * dy <= radius_squared) {
                continue;
            }
            set_pixel(x, y, top_background_rgb);
            set_pixel(size - 1U - x, y, top_background_rgb);
            set_pixel(x, size - 1U - y, bottom_background_rgb);
            set_pixel(size - 1U - x, size - 1U - y, bottom_background_rgb);
        }
    }
}

}  // namespace micropixel::host_ui::lvgl::square_common
