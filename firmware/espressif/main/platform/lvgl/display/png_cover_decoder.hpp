#pragma once

#include <cstdint>

namespace micropixel::platform::lvgl {

// Decode an arbitrary-size, non-interlaced PNG into one bounded RGB888 cover.
// Rows are streamed so source resolution does not require a full-size PSRAM
// allocation. The source is center-cropped to the target aspect ratio.
[[nodiscard]] bool DecodePngCoverRgb888(const uint8_t* source, uint32_t source_size, uint32_t declared_width,
                                        uint32_t declared_height, uint8_t* destination, uint32_t target_width,
                                        uint32_t target_height, uint32_t target_stride, uint32_t background_rgb888);

}  // namespace micropixel::platform::lvgl
