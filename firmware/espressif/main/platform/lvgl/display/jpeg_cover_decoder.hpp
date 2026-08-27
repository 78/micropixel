#pragma once

#include <cstdint>

namespace micropixel::platform::lvgl {

// Decode a color JPEG with the ESP32-P4 JPEG engine, then center-crop and
// scale it into LVGL's little-endian BGR888 Hall-cover layout.
[[nodiscard]] bool DecodeJpegCoverRgb888(const uint8_t* source, uint32_t source_size, uint32_t declared_width,
                                         uint32_t declared_height, uint8_t* destination, uint32_t target_width,
                                         uint32_t target_height, uint32_t target_stride);

}  // namespace micropixel::platform::lvgl
