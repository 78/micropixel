// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace micropixel::platform::lvgl {

// Caller supplies an in-bounds glyph coordinate and a valid bitmap. A8 is
// already coverage; packed formats retain LVGL's bit order and scaling.
inline uint8_t GlyphCoverage(const uint8_t* bitmap, const lv_font_glyph_dsc_t& glyph, uint32_t x, uint32_t y) {
    if (glyph.format == LV_FONT_GLYPH_FORMAT_A8) {
        const uint32_t stride = glyph.stride == 0U ? glyph.box_w : glyph.stride;
        return bitmap[static_cast<size_t>(y) * stride + x];
    }
    const uint32_t bits_per_pixel = static_cast<uint32_t>(glyph.format);
    if (bits_per_pixel == 0U || bits_per_pixel > 8U) {
        return 0U;
    }
    const uint64_t bit_offset = glyph.stride == 0U ? (static_cast<uint64_t>(y) * glyph.box_w + x) * bits_per_pixel
                                                   : static_cast<uint64_t>(y) * glyph.stride * 8U + x * bits_per_pixel;
    const uint64_t total_bits = glyph.stride == 0U ? static_cast<uint64_t>(glyph.box_w) * glyph.box_h * bits_per_pixel
                                                   : static_cast<uint64_t>(glyph.stride) * glyph.box_h * 8U;
    const uint64_t total_bytes = (total_bits + 7U) / 8U;
    const uint32_t byte_offset = static_cast<uint32_t>(bit_offset / 8U);
    const uint32_t intra_byte = static_cast<uint32_t>(bit_offset & 7U);
    const uint16_t window = static_cast<uint16_t>(bitmap[byte_offset]) << 8U |
                            (byte_offset + 1U < total_bytes ? bitmap[byte_offset + 1U] : 0U);
    const uint32_t shift = 16U - intra_byte - bits_per_pixel;
    const uint32_t mask = (1U << bits_per_pixel) - 1U;
    const uint32_t value = (window >> shift) & mask;
    return static_cast<uint8_t>((value * 255U + mask / 2U) / mask);
}

}  // namespace micropixel::platform::lvgl
