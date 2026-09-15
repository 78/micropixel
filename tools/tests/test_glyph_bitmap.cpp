// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <iostream>

#include "platform/lvgl/fonts/glyph_bitmap.hpp"

int main() {
    using micropixel::platform::lvgl::GlyphCoverage;
    // Read all 256 A8 values, both tightly packed and with row padding.
    std::array<uint8_t, 17U * 16U> bitmap{};
    lv_font_glyph_dsc_t glyph{};
    glyph.box_w = glyph.box_h = 16U;
    glyph.format = LV_FONT_GLYPH_FORMAT_A8;
    for (uint16_t stride : {0U, 17U}) {
        glyph.stride = stride;
        const size_t row_bytes = stride == 0U ? 16U : stride;
        bitmap.fill(231U);
        for (size_t y = 0; y < 16U; ++y) {
            for (size_t x = 0; x < 16U; ++x) bitmap[y * row_bytes + x] = static_cast<uint8_t>(y * 16U + x);
        }
        for (uint32_t y = 0; y < 16U; ++y) {
            for (uint32_t x = 0; x < 16U; ++x) {
                assert(GlyphCoverage(bitmap.data(), glyph, x, y) == y * 16U + x);
            }
        }
    }
    // Independent bit-by-bit encoder exercises cross-byte and cross-row packed
    // pixels, row padding, and the last byte (including 3bpp).
    for (uint32_t bpp : {1U, 2U, 3U, 4U}) {
        glyph.format = static_cast<lv_font_glyph_format_t>(bpp);
        glyph.box_w = 7U;
        glyph.box_h = 5U;
        const uint32_t mask = (1U << bpp) - 1U;
        for (uint16_t stride : {0U, 5U}) {
            glyph.stride = stride;
            bitmap.fill(0U);
            for (uint32_t i = 0U; i < 35U; ++i) {
                const uint32_t offset = stride == 0U ? i * bpp : (i / 7U) * stride * 8U + (i % 7U) * bpp;
                const uint32_t value = i & mask;
                for (uint32_t bit = 0; bit < bpp; ++bit) {
                    if ((value >> (bpp - 1U - bit)) & 1U) {
                        bitmap[(offset + bit) / 8U] |= 1U << (7U - (offset + bit) % 8U);
                    }
                }
            }
            for (uint32_t i = 0U; i < 35U; ++i) {
                assert(GlyphCoverage(bitmap.data(), glyph, i % 7U, i / 7U) == ((i & mask) * 255U + mask / 2U) / mask);
            }
        }
    }
    glyph.format = LV_FONT_GLYPH_FORMAT_NONE;
    assert(GlyphCoverage(nullptr, glyph, 0U, 0U) == 0U);
    std::cout << "Glyph coverage: A8 values/stride and packed A1/A2/A3/A4 passed\n";
}
