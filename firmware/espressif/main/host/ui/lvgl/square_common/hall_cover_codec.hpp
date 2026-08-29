#pragma once

#include <cstdint>

#include "host/ui/system_ui.hpp"

namespace micropixel::host_ui::lvgl::square_common {

[[nodiscard]] constexpr uint32_t HallCoverStride(uint32_t size) { return size * 3U; }
[[nodiscard]] constexpr uint32_t HallCoverBytes(uint32_t size) { return HallCoverStride(size) * size; }

void MaskHallCoverRgb888(uint8_t* destination, uint32_t size, uint32_t corner_radius, uint32_t top_background_rgb,
                         uint32_t bottom_background_rgb);

[[nodiscard]] bool DecodeHallCoverRgb888(const host_ui::HallCoverModel& source, uint32_t target_size,
                                         uint32_t corner_radius, uint32_t top_background_rgb,
                                         uint32_t bottom_background_rgb, uint8_t* destination);

}  // namespace micropixel::host_ui::lvgl::square_common
