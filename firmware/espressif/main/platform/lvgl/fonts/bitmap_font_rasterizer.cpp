#include "platform/lvgl/fonts/bitmap_font_rasterizer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace micropixel::platform::lvgl {
namespace {

struct Rgb final {
    uint8_t red{};
    uint8_t green{};
    uint8_t blue{};
};

bool CopyTerminated(const char* text, uint16_t text_length,
                    char (&terminated)[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]) {
    if (text == nullptr || text_length == 0U || text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES) {
        return false;
    }
    std::memcpy(terminated, text, text_length);
    terminated[text_length] = '\0';
    return true;
}

bool NextCodepoint(const char* text, uint16_t length, uint32_t& offset, uint32_t& codepoint) {
    if (offset >= length) {
        codepoint = 0U;
        return true;
    }
    const uint8_t first = static_cast<uint8_t>(text[offset++]);
    if (first == 0U) {
        return false;
    }
    if (first < 0x80U) {
        codepoint = first;
        return true;
    }
    uint32_t remaining = 0U;
    uint32_t minimum = 0U;
    if ((first & 0xe0U) == 0xc0U) {
        remaining = 1U;
        codepoint = first & 0x1fU;
        minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
        remaining = 2U;
        codepoint = first & 0x0fU;
        minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
        remaining = 3U;
        codepoint = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return false;
    }
    if (remaining > length - offset) {
        return false;
    }
    for (uint32_t index = 0U; index < remaining; ++index) {
        const uint8_t next = static_cast<uint8_t>(text[offset++]);
        if ((next & 0xc0U) != 0x80U) {
            return false;
        }
        codepoint = (codepoint << 6U) | (next & 0x3fU);
    }
    return codepoint >= minimum && codepoint <= 0x10ffffU && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

bool ValidDestination(graphics::PixelSurface destination) {
    const uint32_t bytes_per_pixel = destination.format == graphics::SurfacePixelFormat::kBgr888
                                         ? 3U
                                         : (destination.format == graphics::SurfacePixelFormat::kRgb565 ? 2U : 0U);
    if (destination.pixels == nullptr || destination.width == 0U || destination.height == 0U || bytes_per_pixel == 0U ||
        destination.width > UINT32_MAX - destination.origin_x ||
        destination.origin_x + destination.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = (destination.origin_x + destination.width) * bytes_per_pixel;
    const uint64_t required =
        static_cast<uint64_t>(destination.stride) * (destination.origin_y + destination.height - 1U) + row_bytes;
    return destination.stride >= row_bytes && required <= destination.size;
}

uint8_t Expand5(uint16_t value) { return static_cast<uint8_t>((value * 255U + 15U) / 31U); }
uint8_t Expand6(uint16_t value) { return static_cast<uint8_t>((value * 255U + 31U) / 63U); }
uint16_t Compress5(uint8_t value) { return static_cast<uint16_t>((value * 31U + 127U) / 255U); }
uint16_t Compress6(uint8_t value) { return static_cast<uint16_t>((value * 63U + 127U) / 255U); }

Rgb ReadPixel(const uint8_t* pixel, graphics::SurfacePixelFormat format) {
    if (format == graphics::SurfacePixelFormat::kBgr888) {
        return {.red = pixel[2], .green = pixel[1], .blue = pixel[0]};
    }
    uint16_t packed = 0U;
    std::memcpy(&packed, pixel, sizeof(packed));
    return {
        .red = Expand5(static_cast<uint16_t>((packed >> 11U) & 0x1fU)),
        .green = Expand6(static_cast<uint16_t>((packed >> 5U) & 0x3fU)),
        .blue = Expand5(static_cast<uint16_t>(packed & 0x1fU)),
    };
}

void WritePixel(uint8_t* pixel, graphics::SurfacePixelFormat format, Rgb color) {
    if (format == graphics::SurfacePixelFormat::kBgr888) {
        pixel[0] = color.blue;
        pixel[1] = color.green;
        pixel[2] = color.red;
        return;
    }
    const uint16_t packed =
        static_cast<uint16_t>((Compress5(color.red) << 11U) | (Compress6(color.green) << 5U) | Compress5(color.blue));
    std::memcpy(pixel, &packed, sizeof(packed));
}

uint8_t BlendChannel(uint8_t foreground, uint8_t background, uint8_t opacity) {
    return static_cast<uint8_t>(
        (static_cast<uint32_t>(foreground) * opacity + static_cast<uint32_t>(background) * (255U - opacity) + 127U) /
        255U);
}

uint8_t Coverage(const uint8_t* bitmap, const lv_font_glyph_dsc_t& glyph, uint32_t x, uint32_t y) {
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

bool DrawGlyph(graphics::PixelSurface destination, int32_t x, int32_t y, Rgb foreground, const lv_font_t* font,
               lv_font_glyph_dsc_t& glyph) {
    if (glyph.box_w == 0U || glyph.box_h == 0U) {
        return true;
    }
    if (glyph.resolved_font == nullptr || glyph.resolved_font->get_glyph_bitmap == nullptr ||
        glyph.format < LV_FONT_GLYPH_FORMAT_A1 || glyph.format > LV_FONT_GLYPH_FORMAT_A8) {
        return false;
    }
    glyph.req_raw_bitmap = 1U;
    const auto* bitmap = static_cast<const uint8_t*>(glyph.resolved_font->get_glyph_bitmap(&glyph, nullptr));
    glyph.req_raw_bitmap = 0U;
    if (bitmap == nullptr) {
        return false;
    }
    const int32_t glyph_x = x + glyph.ofs_x;
    const int32_t glyph_y = y + (font->line_height - font->base_line) - glyph.box_h - glyph.ofs_y;
    const uint32_t bytes_per_pixel = destination.format == graphics::SurfacePixelFormat::kBgr888 ? 3U : 2U;
    for (uint32_t row = 0U; row < glyph.box_h; ++row) {
        const int32_t destination_y = glyph_y + static_cast<int32_t>(row);
        if (destination_y < 0 || destination_y >= static_cast<int32_t>(destination.height)) {
            continue;
        }
        for (uint32_t column = 0U; column < glyph.box_w; ++column) {
            const int32_t destination_x = glyph_x + static_cast<int32_t>(column);
            if (destination_x < 0 || destination_x >= static_cast<int32_t>(destination.width)) {
                continue;
            }
            const uint8_t opacity = Coverage(bitmap, glyph, column, row);
            if (opacity == 0U) {
                continue;
            }
            uint8_t* pixel =
                destination.pixels +
                static_cast<size_t>(destination.origin_y + static_cast<uint32_t>(destination_y)) * destination.stride +
                (destination.origin_x + static_cast<uint32_t>(destination_x)) * bytes_per_pixel;
            if (opacity == 255U) {
                WritePixel(pixel, destination.format, foreground);
            } else {
                const Rgb background = ReadPixel(pixel, destination.format);
                WritePixel(pixel, destination.format,
                           {.red = BlendChannel(foreground.red, background.red, opacity),
                            .green = BlendChannel(foreground.green, background.green, opacity),
                            .blue = BlendChannel(foreground.blue, background.blue, opacity)});
            }
        }
    }
    return true;
}

}  // namespace

bool BitmapFontRasterizer::Measure(micropixel_font_handle_t font_handle, const char* text, uint16_t text_length,
                                   graphics::RasterTextMetrics& metrics) const {
    const lv_font_t* font = fonts_.ResolveRetainedHandle(font_handle);
    char terminated[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]{};
    if (font == nullptr || !CopyTerminated(text, text_length, terminated)) {
        return false;
    }
    lv_point_t size{};
    lv_text_get_size(&size, terminated, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x < 0 || size.y < 0) {
        return false;
    }
    metrics = {.width = static_cast<uint32_t>(size.x), .height = static_cast<uint32_t>(size.y)};
    return true;
}

bool BitmapFontRasterizer::Draw(graphics::PixelSurface destination, int32_t x, int32_t y, uint32_t rgb888,
                                micropixel_font_handle_t font_handle, const char* text, uint16_t text_length) const {
    const lv_font_t* font = fonts_.ResolveRetainedHandle(font_handle);
    if (!ValidDestination(destination) || font == nullptr || text == nullptr || text_length == 0U ||
        text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES || (rgb888 & 0xff000000U) != 0U) {
        return false;
    }
    const Rgb foreground{.red = static_cast<uint8_t>(rgb888 >> 16U),
                         .green = static_cast<uint8_t>(rgb888 >> 8U),
                         .blue = static_cast<uint8_t>(rgb888)};
    int32_t pen_x = x;
    int32_t pen_y = y;
    uint32_t offset = 0U;
    while (offset < text_length) {
        uint32_t codepoint = 0U;
        if (!NextCodepoint(text, text_length, offset, codepoint)) {
            return false;
        }
        if (codepoint == '\n') {
            pen_x = x;
            pen_y += font->line_height;
            continue;
        }
        uint32_t next_offset = offset;
        uint32_t next_codepoint = 0U;
        if (!NextCodepoint(text, text_length, next_offset, next_codepoint)) {
            return false;
        }
        if (next_codepoint == '\n') {
            next_codepoint = 0U;
        }
        lv_font_glyph_dsc_t glyph{};
        if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, next_codepoint)) {
            return false;
        }
        const bool drawn = DrawGlyph(destination, pen_x, pen_y, foreground, font, glyph);
        pen_x += glyph.adv_w;
        lv_font_glyph_release_draw_data(&glyph);
        if (!drawn) {
            return false;
        }
    }
    return true;
}

}  // namespace micropixel::platform::lvgl
