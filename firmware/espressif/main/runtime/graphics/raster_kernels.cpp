#include "runtime/graphics/raster_kernels.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "device/contracts/graphics.hpp"

namespace micropixel::runtime::raster {
namespace {

constexpr uint32_t kBytesPerRgb565 = 2U;
constexpr uint32_t kFixedShift = 16U;

// Text bytes follow the TEXT header padded to a multiple of 4.
[[nodiscard]] uint32_t PaddedTextBytes(uint32_t text_length) { return (text_length + 3U) & ~3U; }

// Wire size of the record starting at `bytes` (`remaining` bytes are left in
// the list). Fixed for every kind but TEXT, whose header names its payload;
// 0 for an unknown kind or a TEXT header that does not fit.
[[nodiscard]] uint32_t RecordSize(const uint8_t* bytes, uint32_t remaining) {
    const uint8_t type = bytes[0];
    if (type == MICROPIXEL_RASTER_RECORD_TEXT) {
        if (remaining < sizeof(micropixel_raster_text_t)) return 0U;
        micropixel_raster_text_t text{};
        std::memcpy(&text, bytes, sizeof(text));
        return sizeof(text) + PaddedTextBytes(text.text_length);
    }
    switch (type) {
        case MICROPIXEL_RASTER_RECORD_SPAN:
            return sizeof(micropixel_raster_span_t);
        case MICROPIXEL_RASTER_RECORD_COLUMN:
            return sizeof(micropixel_raster_column_t);
        case MICROPIXEL_RASTER_RECORD_SPAN_PAIR:
            return sizeof(micropixel_raster_span_pair_t);
        case MICROPIXEL_RASTER_RECORD_SPRITE:
            return sizeof(micropixel_raster_sprite_t);
        case MICROPIXEL_RASTER_RECORD_IMAGE:
            return sizeof(micropixel_raster_image_t);
        case MICROPIXEL_RASTER_RECORD_RECT:
            return sizeof(micropixel_raster_rect_t);
        case MICROPIXEL_RASTER_RECORD_WARP:
            return sizeof(micropixel_raster_warp_t);
        case MICROPIXEL_RASTER_RECORD_TRIANGLE:
            return sizeof(micropixel_raster_triangle_t);
        case MICROPIXEL_RASTER_RECORD_QUAD:
            return sizeof(micropixel_raster_quad_t);
        default:
            return 0U;
    }
}

// Clips the rectangle (x, y, width, height) to the target. False when nothing
// is left. Outputs are the visible span and how much was cut off the start.
struct ClippedRect final {
    uint32_t x0{};
    uint32_t y0{};
    uint32_t x1{};  // exclusive
    uint32_t y1{};  // exclusive
    uint32_t skip_x{};
    uint32_t skip_y{};
};

[[nodiscard]] bool ClipRect(const Target& target, int32_t x, int32_t y, uint32_t width, uint32_t height,
                            ClippedRect& out) {
    const int64_t right = static_cast<int64_t>(x) + width;
    const int64_t bottom = static_cast<int64_t>(y) + height;
    if (width == 0U || height == 0U || right <= 0 || bottom <= 0 || x >= static_cast<int32_t>(target.width) ||
        y >= static_cast<int32_t>(target.height)) {
        return false;
    }
    out.skip_x = x < 0 ? static_cast<uint32_t>(-x) : 0U;
    out.skip_y = y < 0 ? static_cast<uint32_t>(-y) : 0U;
    out.x0 = x < 0 ? 0U : static_cast<uint32_t>(x);
    out.y0 = y < 0 ? 0U : static_cast<uint32_t>(y);
    out.x1 = right > target.width ? target.width : static_cast<uint32_t>(right);
    out.y1 = bottom > target.height ? target.height : static_cast<uint32_t>(bottom);
    return out.x1 > out.x0 && out.y1 > out.y0;
}

[[nodiscard]] inline uint16_t ByteSwap(uint16_t value) { return static_cast<uint16_t>((value << 8U) | (value >> 8U)); }

[[nodiscard]] inline bool WordAligned(const void* pointer) { return (reinterpret_cast<uintptr_t>(pointer) & 3U) == 0U; }

// Writes `count` copies of `color` starting at `row`; two pixels per store
// once the destination is word aligned, since full-screen fills on a PSRAM
// frame buffer are bound by the number of bus writes.
inline void FillRow(uint16_t* row, uint32_t count, uint16_t color) {
    if (count != 0U && !WordAligned(row)) {
        *row++ = color;
        --count;
    }
    const uint32_t pair = static_cast<uint32_t>(color) | (static_cast<uint32_t>(color) << 16U);
    auto* words = reinterpret_cast<uint32_t*>(row);
    for (; count >= 2U; count -= 2U) *words++ = pair;
    if (count != 0U) *reinterpret_cast<uint16_t*>(words) = color;
}

// Copies `count` canonical RGB565 pixels from `source` to `row`, swapping the
// bytes of each pixel for a swapped panel; two pixels per word once the
// destination is aligned (a texture with an odd stride falls back to bytes).
inline void CopyRowSwapped(uint16_t* row, const uint8_t* source, uint32_t count) {
    if ((reinterpret_cast<uintptr_t>(source) & 1U) != 0U) {
        for (uint32_t index = 0U; index < count; ++index, source += 2U) {
            row[index] = static_cast<uint16_t>((source[0] << 8U) | source[1]);
        }
        return;
    }
    auto swapped = [](uint32_t pair) { return ((pair & 0xFF00FF00U) >> 8U) | ((pair & 0x00FF00FFU) << 8U); };
    if (count != 0U && !WordAligned(row)) {
        *row++ = static_cast<uint16_t>((source[0] << 8U) | source[1]);
        source += 2U;
        --count;
    }
    auto* words = reinterpret_cast<uint32_t*>(row);
    if (WordAligned(source)) {
        const auto* input = reinterpret_cast<const uint32_t*>(source);
        for (; count >= 2U; count -= 2U) *words++ = swapped(*input++);
        source = reinterpret_cast<const uint8_t*>(input);
    } else {
        const auto* input = reinterpret_cast<const uint16_t*>(source);
        for (; count >= 2U; count -= 2U, input += 2U) {
            *words++ = swapped(static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 16U));
        }
        source = reinterpret_cast<const uint8_t*>(input);
    }
    if (count != 0U) *reinterpret_cast<uint16_t*>(words) = static_cast<uint16_t>((source[0] << 8U) | source[1]);
}

// Blends `color` over `dst` in canonical RGB565 with `alpha` in 0..256.
// Red and blue share one multiply: with the coverage reduced to 1/64 steps
// (the panel's 5/6-bit channels cannot show finer blends) the blue lane's
// product stays below bit 11 and never carries into red. Two multiplies per
// pixel instead of three matter on full-screen HUD cards.
[[nodiscard]] inline uint16_t Blend565(uint16_t dst, uint16_t color, uint32_t alpha) {
    const uint32_t coverage = (alpha + 2U) >> 2U;  // 0..64
    const uint32_t inverse = 64U - coverage;
    // Round to nearest: half a step added to each lane (blue's half fits under bit 11).
    const uint32_t rb = ((dst & 0xF81FU) * inverse + (color & 0xF81FU) * coverage + 0x10020U) >> 6U;
    const uint32_t g = ((dst & 0x07E0U) * inverse + (color & 0x07E0U) * coverage + 0x400U) >> 6U;
    return static_cast<uint16_t>((rb & 0xF81FU) | (g & 0x07E0U));
}

// Adds `color` to `dst` in canonical RGB565, each channel saturating at its
// maximum. Additive sprites (glows, light pools) stack on a black background
// the way light does; overlapping sprites sum instead of overwriting.
[[nodiscard]] inline uint16_t AddSaturate565(uint16_t dst, uint16_t color) {
    uint32_t r = (dst >> 11U) + (color >> 11U);
    uint32_t g = ((dst >> 5U) & 0x3FU) + ((color >> 5U) & 0x3FU);
    uint32_t b = (dst & 0x1FU) + (color & 0x1FU);
    r = r > 0x1FU ? 0x1FU : r;
    g = g > 0x3FU ? 0x3FU : g;
    b = b > 0x1FU ? 0x1FU : b;
    return static_cast<uint16_t>((r << 11U) | (g << 5U) | b);
}

[[nodiscard]] const Texture* SlotWithLayout(const Resources& resources, uint8_t slot, uint8_t layout) {
    const Texture* texture = resources.TextureAt(slot);
    return texture != nullptr && texture->pixels != nullptr && texture->layout == layout ? texture : nullptr;
}

// A missing palette slot is STALE_STATE (the App has not uploaded it yet); a
// light level the slot does not have is INVALID_ARGUMENT.
[[nodiscard]] int32_t CheckLight(const Resources& resources, uint8_t palette_slot, uint32_t light_level) {
    const Palette* palette = resources.PaletteAt(palette_slot);
    if (palette == nullptr) return MICROPIXEL_STATUS_STALE_STATE;
    return light_level < palette->light_levels ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INVALID_ARGUMENT;
}

[[nodiscard]] int32_t ValidateColumn(const micropixel_raster_column_t& column, const Target& target,
                                     const Resources& resources) {
    if ((column.flags & ~MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0) != 0U || column.reserved0[0] != 0U ||
        column.reserved0[1] != 0U || column.reserved0[2] != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (column.x >= target.width || column.y0 < 0 || column.y1 < column.y0 ||
        column.y1 >= static_cast<int32_t>(target.height)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const int32_t light = CheckLight(resources, column.palette_slot, column.light_level);
    if (light != MICROPIXEL_STATUS_OK) return light;
    const Texture* texture = SlotWithLayout(resources, column.texture_slot, MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR);
    if (texture == nullptr || column.u >= texture->width) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateSpanPair(const micropixel_raster_span_pair_t& span, const Target& target,
                                       const Resources& resources) {
    if (span.flags != 0U || span.reserved0[0] != 0U || span.reserved0[1] != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (span.y_floor >= target.height || span.y_ceiling >= target.height || span.x1 < span.x0 ||
        span.x1 >= target.width) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const int32_t light = CheckLight(resources, span.palette_slot, span.light_level);
    if (light != MICROPIXEL_STATUS_OK) return light;
    if (SlotWithLayout(resources, span.floor_texture_slot, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR) == nullptr ||
        SlotWithLayout(resources, span.ceiling_texture_slot, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR) == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateSpan(const micropixel_raster_span_t& span, const Target& target,
                                   const Resources& resources) {
    if (span.flags != 0U || span.reserved0 != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (span.y >= target.height || span.x1 < span.x0 || span.x1 >= target.width) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const int32_t light = CheckLight(resources, span.palette_slot, span.light_level);
    if (light != MICROPIXEL_STATUS_OK) return light;
    if (SlotWithLayout(resources, span.texture_slot, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR) == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    return MICROPIXEL_STATUS_OK;
}

// Well-formed UTF-8 without NUL bytes: the text rasterizer refuses anything
// else mid-list, which would leave a half-drawn frame.
[[nodiscard]] bool ValidUtf8(const uint8_t* text, uint32_t length) {
    uint32_t offset = 0U;
    while (offset < length) {
        const uint8_t first = text[offset++];
        if (first == 0U) return false;
        if (first < 0x80U) continue;
        uint32_t remaining = 0U;
        uint32_t codepoint = 0U;
        uint32_t minimum = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            remaining = 1U;
            codepoint = first & 0x1FU;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            remaining = 2U;
            codepoint = first & 0x0FU;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            remaining = 3U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (remaining > length - offset) return false;
        for (uint32_t index = 0U; index < remaining; ++index) {
            const uint8_t next = text[offset++];
            if ((next & 0xC0U) != 0x80U) return false;
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
    }
    return true;
}

// `bytes` points at the record header; RecordSize accepted it, so the padded
// payload is known to fit in the list.
[[nodiscard]] int32_t ValidateText(const uint8_t* bytes, const Resources& resources) {
    micropixel_raster_text_t text{};
    std::memcpy(&text, bytes, sizeof(text));
    if (text.flags != 0U || text.reserved0 != 0U || text.text_length == 0U ||
        text.text_length > micropixel::device::graphics_limits::kMaxTextBytes || text.font_handle == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t* payload = bytes + sizeof(text);
    for (uint32_t index = text.text_length; index < PaddedTextBytes(text.text_length); ++index) {
        if (payload[index] != 0U) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (!ValidUtf8(payload, text.text_length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (resources.validate_text == nullptr || resources.draw_text == nullptr) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    return resources.validate_text(resources.text_context, text.font_handle, reinterpret_cast<const char*>(payload),
                                   text.text_length)
               ? MICROPIXEL_STATUS_OK
               : MICROPIXEL_STATUS_INVALID_ARGUMENT;
}

[[nodiscard]] int32_t ValidateSprite(const micropixel_raster_sprite_t& sprite, const Resources& resources) {
    if ((sprite.flags & ~(MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 | MICROPIXEL_RASTER_SPRITE_SOLID_COLOR |
                          MICROPIXEL_RASTER_SPRITE_ADDITIVE)) != 0U ||
        sprite.reserved0 != 0U || sprite.width == 0U || sprite.height == 0U || sprite.source_width == 0U ||
        sprite.source_height == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if ((sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) == 0U) {
        const int32_t light = CheckLight(resources, sprite.palette_slot, sprite.light_level);
        if (light != MICROPIXEL_STATUS_OK) return light;
    }
    const Texture* texture = SlotWithLayout(resources, sprite.texture_slot, MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR);
    if (texture == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (static_cast<uint32_t>(sprite.source_x) + sprite.source_width > texture->width ||
        static_cast<uint32_t>(sprite.source_y) + sprite.source_height > texture->height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateWarp(const micropixel_raster_warp_t& warp, const Resources& resources) {
    if ((warp.flags & ~MICROPIXEL_RASTER_WARP_FILL_SKIPPED) != 0U ||
        warp.u_fraction_bits > MICROPIXEL_RASTER_WARP_MAX_U_FRACTION_BITS) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const WarpMap* map = resources.WarpAt(warp.warp_slot);
    if (map == nullptr) return MICROPIXEL_STATUS_STALE_STATE;
    const int32_t light = CheckLight(resources, warp.palette_slot, map->max_light);
    if (light != MICROPIXEL_STATUS_OK) return light;
    const Texture* texture = SlotWithLayout(resources, warp.texture_slot, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR);
    if (texture == nullptr) return MICROPIXEL_STATUS_NOT_FOUND;
    // The kernel wraps u/v with masks, so any texture that is not a power of
    // two (or is wider than the 12-bit entry fields, fraction bits included)
    // cannot be sampled safely.
    if (texture->log2_width == UINT8_MAX || texture->log2_height == UINT8_MAX ||
        (static_cast<uint32_t>(texture->width) << warp.u_fraction_bits) > MICROPIXEL_RASTER_WARP_MAX_TEXTURE_SIZE ||
        texture->height > MICROPIXEL_RASTER_WARP_MAX_TEXTURE_SIZE) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateImage(const micropixel_raster_image_t& image, const Resources& resources) {
    if (image.reserved0 || !image.width || !image.height || !image.source_width || !image.source_height)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    device::BitmapView texture{};
    if (!image.texture_handle || !resources.resolve_texture ||
        !resources.resolve_texture(resources.texture_context, image.texture_handle, texture))
        return MICROPIXEL_STATUS_NOT_FOUND;
    const uint32_t bytes = texture.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565     ? 2
                           : texture.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888   ? 3
                           : texture.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4
                                                                                      : 0;
    if (!bytes) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (!texture.data || !texture.width || !texture.height ||
        static_cast<uint64_t>(texture.width) * bytes > texture.stride ||
        static_cast<uint64_t>(texture.stride) * texture.height > texture.size ||
        static_cast<uint32_t>(image.source_x) + image.source_width > texture.width ||
        static_cast<uint32_t>(image.source_y) + image.source_height > texture.height)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    return MICROPIXEL_STATUS_OK;
}

// Shared by TRIANGLE and QUAD: flags, padding, every corner light below the
// palette's level count and (unless FLAT_COLOR) a ROW_MAJOR power-of-two
// texture the span kernel can mask-wrap on. Coordinates are unconstrained; the
// kernel clips.
[[nodiscard]] int32_t ValidatePolygon(uint8_t flags, uint8_t texture_slot, uint8_t palette_slot,
                                      const micropixel_raster_vertex_t* vertices, uint32_t vertex_count,
                                      const Resources& resources) {
    constexpr uint8_t kKnownFlags = MICROPIXEL_RASTER_POLYGON_TRANSPARENT_INDEX0 | MICROPIXEL_RASTER_POLYGON_FLAT_COLOR;
    if ((flags & ~kKnownFlags) != 0U) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    uint32_t max_light = 0U;
    for (uint32_t index = 0U; index < vertex_count; ++index) {
        if (vertices[index].reserved0 != 0U) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        max_light = std::max<uint32_t>(max_light, vertices[index].light);
    }
    const int32_t light = CheckLight(resources, palette_slot, max_light);
    if (light != MICROPIXEL_STATUS_OK) return light;
    if ((flags & MICROPIXEL_RASTER_POLYGON_FLAT_COLOR) != 0U) return MICROPIXEL_STATUS_OK;
    const Texture* texture = SlotWithLayout(resources, texture_slot, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR);
    if (texture == nullptr) return MICROPIXEL_STATUS_NOT_FOUND;
    if (texture->log2_width == UINT8_MAX || texture->log2_height == UINT8_MAX) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateTriangle(const micropixel_raster_triangle_t& triangle, const Resources& resources) {
    if (triangle.reserved0 != 0U) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    return ValidatePolygon(triangle.flags, triangle.texture_slot, triangle.palette_slot, triangle.vertices, 3U,
                           resources);
}

[[nodiscard]] int32_t ValidateQuad(const micropixel_raster_quad_t& quad, const Resources& resources) {
    return ValidatePolygon(quad.flags, quad.texture_slot, quad.palette_slot, quad.vertices, 4U, resources);
}

// Twice the signed area of a polygon in 12.4 units (exact in 64 bits), so a
// polygon's pixel count is |area| / 512 and its winding is the sign.
[[nodiscard]] int64_t PolygonArea2(const micropixel_raster_vertex_t* vertices, uint32_t vertex_count) {
    int64_t area = 0;
    for (uint32_t index = 0U; index < vertex_count; ++index) {
        const micropixel_raster_vertex_t& a = vertices[index];
        const micropixel_raster_vertex_t& b = vertices[index + 1U == vertex_count ? 0U : index + 1U];
        area += static_cast<int64_t>(a.x) * b.y - static_cast<int64_t>(b.x) * a.y;
    }
    return area;
}

[[nodiscard]] int32_t ValidateRect(const micropixel_raster_rect_t& rect) {
    if (rect.flags != 0U || rect.reserved0 != 0U || rect.reserved1 != 0U || rect.opacity == 0U || rect.width == 0U ||
        rect.height == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] inline uint16_t* Row(const Target& target, uint32_t y) {
    return reinterpret_cast<uint16_t*>(target.pixels + y * target.pitch);
}

}  // namespace

uint8_t Log2Exact(uint32_t power_of_two) {
    if (power_of_two == 0U || (power_of_two & (power_of_two - 1U)) != 0U) return UINT8_MAX;
    uint8_t result = 0U;
    while (power_of_two > 1U) {
        power_of_two >>= 1U;
        ++result;
    }
    return result;
}

int32_t ValidateDrawList(const uint8_t* bytes, uint32_t length, const Target& target, const Resources& resources,
                         micropixel_raster_header_t& header_out) {
    if (bytes == nullptr || length < sizeof(micropixel_raster_header_t) ||
        length > micropixel::device::graphics_limits::kMaxRasterBytes) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    micropixel_raster_header_t header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != MICROPIXEL_GRAPHICS_RASTER_MAGIC || header.total_size != length || header.flags != 0U ||
        header.reserved0 != 0U || header.record_count == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (target.width == 0U || target.height == 0U || (target.pitch % kBytesPerRgb565) != 0U ||
        target.pitch < static_cast<uint32_t>(target.width) * kBytesPerRgb565) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    // RECT, IMAGE and SOLID_COLOR sprites need no palette; every other record
    // names a palette slot, and a missing slot is STALE_STATE (not uploaded
    // yet) rather than a malformed record.
    uint32_t offset = sizeof(header);
    for (uint32_t index = 0U; index < header.record_count; ++index) {
        if (offset >= length) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        const uint8_t type = bytes[offset];  // every record starts with its type byte
        const uint32_t record_size = RecordSize(bytes + offset, length - offset);
        if (record_size == 0U || record_size > length - offset) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        int32_t status = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (type == MICROPIXEL_RASTER_RECORD_SPAN) {
            micropixel_raster_span_t span{};
            std::memcpy(&span, bytes + offset, sizeof(span));
            status = ValidateSpan(span, target, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_TEXT) {
            status = ValidateText(bytes + offset, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_COLUMN) {
            micropixel_raster_column_t column{};
            std::memcpy(&column, bytes + offset, sizeof(column));
            status = ValidateColumn(column, target, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_SPAN_PAIR) {
            micropixel_raster_span_pair_t span{};
            std::memcpy(&span, bytes + offset, sizeof(span));
            status = ValidateSpanPair(span, target, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_SPRITE) {
            micropixel_raster_sprite_t sprite{};
            std::memcpy(&sprite, bytes + offset, sizeof(sprite));
            status = ValidateSprite(sprite, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_IMAGE) {
            micropixel_raster_image_t image{};
            std::memcpy(&image, bytes + offset, sizeof(image));
            status = ValidateImage(image, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_WARP) {
            micropixel_raster_warp_t warp{};
            std::memcpy(&warp, bytes + offset, sizeof(warp));
            status = ValidateWarp(warp, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_TRIANGLE) {
            micropixel_raster_triangle_t triangle{};
            std::memcpy(&triangle, bytes + offset, sizeof(triangle));
            status = ValidateTriangle(triangle, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_QUAD) {
            micropixel_raster_quad_t quad{};
            std::memcpy(&quad, bytes + offset, sizeof(quad));
            status = ValidateQuad(quad, resources);
        } else {
            micropixel_raster_rect_t rect{};
            std::memcpy(&rect, bytes + offset, sizeof(rect));
            status = ValidateRect(rect);
        }
        if (status != MICROPIXEL_STATUS_OK) {
            return status;
        }
        offset += record_size;
    }
    if (offset != length) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    header_out = header;
    return MICROPIXEL_STATUS_OK;
}

// Additive mode: 0 replaces, 1 adds in canonical order, 2 adds on a
// byte-swapped panel (source and target both hold panel order).
template <bool kTransparent, int kAdditive>
void DrawSpriteRows(const Target& target, const Texture& texture, const uint16_t* lit,
                    const micropixel_raster_sprite_t& sprite, const ClippedRect& clip, bool solid,
                    uint16_t solid_color) {
    // 16.16 texel steps per destination pixel; sampling starts at the centre
    // of the first visible pixel so a clipped sprite keeps its phase.
    const uint32_t u_step = (static_cast<uint32_t>(sprite.source_width) << kFixedShift) / sprite.width;
    const uint32_t v_step = (static_cast<uint32_t>(sprite.source_height) << kFixedShift) / sprite.height;
    uint32_t v = clip.skip_y * v_step + (v_step >> 1U);
    const uint32_t u_start = clip.skip_x * u_step + (u_step >> 1U);
    const uint32_t texture_height = texture.height;
    for (uint32_t y = clip.y0; y < clip.y1; ++y, v += v_step) {
        const uint8_t* texels = texture.pixels + (static_cast<uint32_t>(sprite.source_y) + (v >> kFixedShift));
        uint16_t* row = Row(target, y) + clip.x0;
        uint32_t u = u_start;
        for (uint32_t x = clip.x0; x < clip.x1; ++x, u += u_step) {
            const uint8_t texel =
                texels[(static_cast<uint32_t>(sprite.source_x) + (u >> kFixedShift)) * texture_height];
            if constexpr (kTransparent) {
                if (texel == 0U) continue;
            }
            const uint16_t source = solid ? solid_color : lit[texel];
            uint16_t& out = row[x - clip.x0];
            if constexpr (kAdditive == 0) {
                out = source;
            } else if constexpr (kAdditive == 1) {
                out = AddSaturate565(out, source);
            } else {
                out = ByteSwap(AddSaturate565(ByteSwap(out), ByteSwap(source)));
            }
        }
    }
}

void DrawSprite(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_sprite_t& sprite) {
    ClippedRect clip{};
    if (!ClipRect(target, sprite.x, sprite.y, sprite.width, sprite.height, clip)) {
        return;
    }
    const bool transparent = (sprite.flags & MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0) != 0U;
    const bool solid = (sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) != 0U;
    const bool additive = (sprite.flags & MICROPIXEL_RASTER_SPRITE_ADDITIVE) != 0U;
    const uint16_t solid_color = target.byte_swapped ? ByteSwap(sprite.color) : sprite.color;
    const int mode = additive ? (target.byte_swapped ? 2 : 1) : 0;
    if (transparent) {
        if (mode == 0) DrawSpriteRows<true, 0>(target, texture, lit, sprite, clip, solid, solid_color);
        if (mode == 1) DrawSpriteRows<true, 1>(target, texture, lit, sprite, clip, solid, solid_color);
        if (mode == 2) DrawSpriteRows<true, 2>(target, texture, lit, sprite, clip, solid, solid_color);
    } else {
        if (mode == 0) DrawSpriteRows<false, 0>(target, texture, lit, sprite, clip, solid, solid_color);
        if (mode == 1) DrawSpriteRows<false, 1>(target, texture, lit, sprite, clip, solid, solid_color);
        if (mode == 2) DrawSpriteRows<false, 2>(target, texture, lit, sprite, clip, solid, solid_color);
    }
}

void DrawRect(const Target& target, const micropixel_raster_rect_t& rect) {
    ClippedRect clip{};
    if (!ClipRect(target, rect.x, rect.y, rect.width, rect.height, clip)) {
        return;
    }
    const uint32_t count = clip.x1 - clip.x0;
    if (rect.opacity == 0xFFU) {
        const uint16_t color = target.byte_swapped ? ByteSwap(rect.color) : rect.color;
        for (uint32_t y = clip.y0; y < clip.y1; ++y) FillRow(Row(target, y) + clip.x0, count, color);
        return;
    }
    // Blend in canonical order; swapped targets are converted per pixel.
    const uint32_t alpha = static_cast<uint32_t>(rect.opacity) + 1U;  // 1..254 -> 2..255 (255 handled above)
    for (uint32_t y = clip.y0; y < clip.y1; ++y) {
        uint16_t* row = Row(target, y) + clip.x0;
        if (target.byte_swapped) {
            for (uint32_t x = 0U; x < count; ++x) {
                row[x] = ByteSwap(Blend565(ByteSwap(row[x]), rect.color, alpha));
            }
        } else {
            for (uint32_t x = 0U; x < count; ++x) {
                row[x] = Blend565(row[x], rect.color, alpha);
            }
        }
    }
}

void DrawColumn(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_column_t& column) {
    const uint8_t* texels = texture.pixels + static_cast<uint32_t>(column.u) * texture.height;
    if (texture.log2_height == UINT8_MAX) {
        uint32_t v = static_cast<uint32_t>(column.v_start);
        for (int32_t y = column.y0; y <= column.y1; ++y, v += static_cast<uint32_t>(column.v_step)) {
            const int32_t row = std::bit_cast<int32_t>(v) >> kFixedShift;
            int32_t wrapped = row % static_cast<int32_t>(texture.height);
            if (wrapped < 0) wrapped += texture.height;
            const uint8_t texel = texels[wrapped];
            if (texel != 0U || (column.flags & MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0) == 0U) {
                Row(target, static_cast<uint32_t>(y))[column.x] = lit[texel];
            }
        }
        return;
    }
    const uint32_t mask = static_cast<uint32_t>(texture.height) - 1U;
    uint8_t* row = target.pixels + static_cast<uint32_t>(column.y0) * target.pitch +
                   static_cast<uint32_t>(column.x) * kBytesPerRgb565;
    const uint32_t pitch = target.pitch;
    uint32_t v = static_cast<uint32_t>(column.v_start);
    const uint32_t step = static_cast<uint32_t>(column.v_step);
    const int32_t count = column.y1 - column.y0 + 1;
    if ((column.flags & MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0) != 0U) {
        for (int32_t index = 0; index < count; ++index) {
            const uint8_t texel = texels[(v >> kFixedShift) & mask];
            if (texel != 0U) {
                *reinterpret_cast<uint16_t*>(row) = lit[texel];
            }
            row += pitch;
            v += step;
        }
        return;
    }
    // Four independent texel chains per iteration: the in-order core would
    // otherwise stall on every load-use pair (texel byte, then palette entry).
    int32_t index = 0;
    for (; index + 4 <= count; index += 4) {
        const uint8_t t0 = texels[(v >> kFixedShift) & mask];
        const uint8_t t1 = texels[((v + step) >> kFixedShift) & mask];
        const uint8_t t2 = texels[((v + 2U * step) >> kFixedShift) & mask];
        const uint8_t t3 = texels[((v + 3U * step) >> kFixedShift) & mask];
        const uint16_t c0 = lit[t0];
        const uint16_t c1 = lit[t1];
        const uint16_t c2 = lit[t2];
        const uint16_t c3 = lit[t3];
        *reinterpret_cast<uint16_t*>(row) = c0;
        *reinterpret_cast<uint16_t*>(row + pitch) = c1;
        *reinterpret_cast<uint16_t*>(row + 2U * pitch) = c2;
        *reinterpret_cast<uint16_t*>(row + 3U * pitch) = c3;
        row += 4U * pitch;
        v += 4U * step;
    }
    for (; index < count; ++index) {
        *reinterpret_cast<uint16_t*>(row) = lit[texels[(v >> kFixedShift) & mask]];
        row += pitch;
        v += step;
    }
}

void DrawSpanPair(const Target& target, const Texture& floor_texture_slot, const Texture& ceiling_texture_slot,
                  const uint16_t* lit, const micropixel_raster_span_pair_t& span) {
    uint16_t* floor = Row(target, span.y_floor) + span.x0;
    uint16_t* ceiling = Row(target, span.y_ceiling) + span.x0;
    const uint32_t count = static_cast<uint32_t>(span.x1) - span.x0 + 1U;
    uint32_t s = static_cast<uint32_t>(span.s);
    uint32_t t = static_cast<uint32_t>(span.t);
    const uint32_t ds = static_cast<uint32_t>(span.ds);
    const uint32_t dt = static_cast<uint32_t>(span.dt);
    if (floor_texture_slot.log2_width == UINT8_MAX || floor_texture_slot.log2_height == UINT8_MAX ||
        ceiling_texture_slot.log2_width == UINT8_MAX || ceiling_texture_slot.log2_height == UINT8_MAX) {
        const auto offset = [](const Texture& texture, uint32_t u, uint32_t v) {
            const uint32_t x = ((u & 0xffffU) * texture.width) >> kFixedShift;
            const uint32_t y = ((v & 0xffffU) * texture.height) >> kFixedShift;
            return y * texture.width + x;
        };
        for (uint32_t index = 0U; index < count; ++index) {
            floor[index] = lit[floor_texture_slot.pixels[offset(floor_texture_slot, s, t)]];
            ceiling[index] = lit[ceiling_texture_slot.pixels[offset(ceiling_texture_slot, s, t)]];
            s += ds;
            t += dt;
        }
        return;
    }
    if (floor_texture_slot.width == ceiling_texture_slot.width &&
        floor_texture_slot.height == ceiling_texture_slot.height) {
        // Common case: one texel index serves both rows.
        const uint32_t shift_s = kFixedShift - floor_texture_slot.log2_width;
        const uint32_t shift_t = kFixedShift - floor_texture_slot.log2_height;
        const uint32_t mask_x = static_cast<uint32_t>(floor_texture_slot.width) - 1U;
        const uint32_t mask_y = static_cast<uint32_t>(floor_texture_slot.height) - 1U;
        const uint32_t log2_width = floor_texture_slot.log2_width;
        const uint8_t* floor_texels = floor_texture_slot.pixels;
        const uint8_t* ceiling_texels = ceiling_texture_slot.pixels;
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t texel = (((t >> shift_t) & mask_y) << log2_width) | ((s >> shift_s) & mask_x);
            floor[index] = lit[floor_texels[texel]];
            ceiling[index] = lit[ceiling_texels[texel]];
            s += ds;
            t += dt;
        }
        return;
    }
    const uint32_t floor_shift_s = kFixedShift - floor_texture_slot.log2_width;
    const uint32_t floor_shift_t = kFixedShift - floor_texture_slot.log2_height;
    const uint32_t ceiling_shift_s = kFixedShift - ceiling_texture_slot.log2_width;
    const uint32_t ceiling_shift_t = kFixedShift - ceiling_texture_slot.log2_height;
    for (uint32_t index = 0U; index < count; ++index) {
        const uint32_t floor_texel =
            (((t >> floor_shift_t) & (floor_texture_slot.height - 1U)) << floor_texture_slot.log2_width) |
            ((s >> floor_shift_s) & (floor_texture_slot.width - 1U));
        const uint32_t ceiling_texel =
            (((t >> ceiling_shift_t) & (ceiling_texture_slot.height - 1U)) << ceiling_texture_slot.log2_width) |
            ((s >> ceiling_shift_s) & (ceiling_texture_slot.width - 1U));
        floor[index] = lit[floor_texture_slot.pixels[floor_texel]];
        ceiling[index] = lit[ceiling_texture_slot.pixels[ceiling_texel]];
        s += ds;
        t += dt;
    }
}

void DrawSpan(const Target& target, const Texture& texture, const uint16_t* lit, const micropixel_raster_span_t& span) {
    uint16_t* row = Row(target, span.y) + span.x0;
    const uint32_t count = static_cast<uint32_t>(span.x1) - span.x0 + 1U;
    uint32_t s = static_cast<uint32_t>(span.s);
    uint32_t t = static_cast<uint32_t>(span.t);
    const uint32_t ds = static_cast<uint32_t>(span.ds);
    const uint32_t dt = static_cast<uint32_t>(span.dt);
    const uint8_t* texels = texture.pixels;
    if (texture.log2_width == UINT8_MAX || texture.log2_height == UINT8_MAX) {
        const uint32_t width = texture.width;
        const uint32_t height = texture.height;
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t x = ((s & 0xffffU) * width) >> kFixedShift;
            const uint32_t y = ((t & 0xffffU) * height) >> kFixedShift;
            row[index] = lit[texels[y * width + x]];
            s += ds;
            t += dt;
        }
        return;
    }
    const uint32_t shift_s = kFixedShift - texture.log2_width;
    const uint32_t shift_t = kFixedShift - texture.log2_height;
    const uint32_t mask_x = static_cast<uint32_t>(texture.width) - 1U;
    const uint32_t mask_y = static_cast<uint32_t>(texture.height) - 1U;
    const uint32_t log2_width = texture.log2_width;
    if (dt == 0U) {
        // A ground row samples one texture row: hoist it and run four texel
        // chains per iteration so the in-order core overlaps the two dependent
        // loads (texel byte, then palette entry) of neighbouring pixels.
        const uint8_t* texel_row = texels + (((t >> shift_t) & mask_y) << log2_width);
        uint32_t index = 0U;
        for (; index + 4U <= count; index += 4U) {
            const uint8_t t0 = texel_row[(s >> shift_s) & mask_x];
            const uint8_t t1 = texel_row[((s + ds) >> shift_s) & mask_x];
            const uint8_t t2 = texel_row[((s + 2U * ds) >> shift_s) & mask_x];
            const uint8_t t3 = texel_row[((s + 3U * ds) >> shift_s) & mask_x];
            row[index] = lit[t0];
            row[index + 1U] = lit[t1];
            row[index + 2U] = lit[t2];
            row[index + 3U] = lit[t3];
            s += 4U * ds;
        }
        for (; index < count; ++index) {
            row[index] = lit[texel_row[(s >> shift_s) & mask_x]];
            s += ds;
        }
        return;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        row[index] = lit[texels[(((t >> shift_t) & mask_y) << log2_width) | ((s >> shift_s) & mask_x)]];
        s += ds;
        t += dt;
    }
}

void DrawImage(const Target& target, const device::BitmapView& texture, const micropixel_raster_image_t& image) {
    ClippedRect clipped{};
    if (!image.opacity || !ClipRect(target, image.x, image.y, image.width, image.height, clipped)) return;
    const uint32_t bytes = texture.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565   ? 2
                           : texture.pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888 ? 3
                                                                                    : 4;
    // Nearest sampling: source column floor(dx * source_width / width) for
    // destination column dx, walked with an integer step and remainder so the
    // inner loop divides nothing.
    const uint32_t count = clipped.x1 - clipped.x0;
    const uint64_t first_numerator = static_cast<uint64_t>(clipped.skip_x) * image.source_width;
    const uint32_t first_sx = image.source_x + static_cast<uint32_t>(first_numerator / image.width);
    const uint32_t first_remainder = static_cast<uint32_t>(first_numerator % image.width);
    const uint32_t step = image.source_width / image.width;
    const uint32_t step_remainder = image.source_width % image.width;
    const bool opaque_copy = image.opacity == 255U && bytes == 2U && image.source_width == image.width;
    // RGB565 textures may already be stored in the panel byte order
    // (device::bitmap_flags::kRgb565ByteSwapped); then a copy is verbatim and
    // blending has to swap the texel back to canonical first.
    const bool texture_swapped = bytes == 2U && (texture.flags & device::bitmap_flags::kRgb565ByteSwapped) != 0U;
    for (uint32_t y = clipped.y0; y < clipped.y1; ++y) {
        const uint32_t sy = image.source_y + (clipped.skip_y + y - clipped.y0) * image.source_height / image.height;
        auto* destination = Row(target, y);
        const uint8_t* source_row = texture.data + sy * texture.stride;
        if (opaque_copy) {
            // Unscaled RGB565 rows (backdrops, HUD cards) are a straight copy
            // unless the texture and the panel disagree on byte order.
            const uint8_t* source = source_row + first_sx * 2U;
            if (target.byte_swapped == texture_swapped) {
                std::memcpy(destination + clipped.x0, source, static_cast<size_t>(count) * 2U);
            } else {
                CopyRowSwapped(destination + clipped.x0, source, count);
            }
            continue;
        }
        uint32_t sx = first_sx;
        uint32_t remainder = first_remainder;
        for (uint32_t x = clipped.x0; x < clipped.x1; ++x) {
            const uint8_t* pixel = source_row + sx * bytes;
            sx += step;
            remainder += step_remainder;
            if (remainder >= image.width) {
                remainder -= image.width;
                ++sx;
            }
            uint16_t color{};
            uint32_t alpha = image.opacity;
            if (bytes == 2) {
                std::memcpy(&color, pixel, 2);
                if (texture_swapped) color = ByteSwap(color);
            } else {
                color = static_cast<uint16_t>(((pixel[2] >> 3) << 11) | ((pixel[1] >> 2) << 5) | (pixel[0] >> 3));
                if (bytes == 4) alpha = (alpha * pixel[3] + 127) / 255;
            }
            if (alpha == 0) continue;
            if (alpha != 255) {
                // Same 1/64-step blend as RECT; alpha 1..254 -> 2..255.
                const uint16_t old = target.byte_swapped ? ByteSwap(destination[x]) : destination[x];
                color = Blend565(old, color, alpha + 1U);
            }
            destination[x] = target.byte_swapped ? ByteSwap(color) : color;
        }
    }
}

uint8_t WarpMaxLight(const uint32_t* entries, uint32_t count) {
    uint32_t max_light = 0U;
    for (uint32_t index = 0U; index < count; ++index) {
        const uint32_t entry = entries[index];
        if ((entry & MICROPIXEL_RASTER_WARP_ENTRY_SKIP) != 0U) continue;
        if ((entry & MICROPIXEL_RASTER_WARP_ENTRY_RESERVED) != 0U) return UINT8_MAX;
        const uint32_t light = (entry >> MICROPIXEL_RASTER_WARP_LIGHT_SHIFT) & MICROPIXEL_RASTER_WARP_LIGHT_MASK;
        if (light > max_light) max_light = light;
    }
    return static_cast<uint8_t>(max_light);
}

bool WarpScanRows(const uint32_t* rows, uint32_t width, uint32_t row_count, uint16_t* spans, uint8_t& max_light) {
    uint32_t light_max = 0U;
    for (uint32_t row = 0U; row < row_count; ++row, rows += width, spans += 2U) {
        uint32_t begin = width;
        uint32_t end = 0U;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint32_t entry = rows[x];
            if ((entry & MICROPIXEL_RASTER_WARP_ENTRY_SKIP) != 0U) continue;
            if ((entry & MICROPIXEL_RASTER_WARP_ENTRY_RESERVED) != 0U) return false;
            const uint32_t light = (entry >> MICROPIXEL_RASTER_WARP_LIGHT_SHIFT) & MICROPIXEL_RASTER_WARP_LIGHT_MASK;
            if (light > light_max) light_max = light;
            if (x < begin) begin = x;
            end = x + 1U;
        }
        spans[0] = static_cast<uint16_t>(begin < end ? begin : width);
        spans[1] = static_cast<uint16_t>(begin < end ? end : width);
    }
    max_light = static_cast<uint8_t>(light_max);
    return true;
}

void WarpRowSpan(const uint32_t* row, uint32_t width, uint16_t& first, uint16_t& last) {
    uint32_t begin = 0U;
    while (begin < width && (row[begin] & MICROPIXEL_RASTER_WARP_ENTRY_SKIP) != 0U) ++begin;
    uint32_t end = width;
    while (end > begin && (row[end - 1U] & MICROPIXEL_RASTER_WARP_ENTRY_SKIP) != 0U) --end;
    first = static_cast<uint16_t>(begin);
    last = static_cast<uint16_t>(end);
}

void DrawWarp(const Target& target, const WarpMap& warp, const Texture& texture, const Palette& palette,
              const micropixel_raster_warp_t& record) {
    ClippedRect clip{};
    if (!ClipRect(target, record.x, record.y, warp.width, warp.height, clip)) {
        return;
    }
    const bool fill = (record.flags & MICROPIXEL_RASTER_WARP_FILL_SKIPPED) != 0U;
    const uint16_t fill_color = target.byte_swapped ? ByteSwap(record.fill_color) : record.fill_color;
    const uint32_t u_mask = static_cast<uint32_t>(texture.width) - 1U;
    const uint32_t v_mask = static_cast<uint32_t>(texture.height) - 1U;
    const uint32_t log2_width = texture.log2_width;
    // u sits in the low bits, so the offset can be added to the whole entry,
    // shifted past its fraction bits and masked: the carry out of the u field
    // lands in bits the v/light extraction never reads, and the mask never
    // reaches them (width << u_fraction_bits fits the field). v needs its own
    // add after the shift.
    const uint32_t u_offset = record.u_offset;
    const uint32_t u_shift = record.u_fraction_bits;
    const uint32_t v_offset = record.v_offset;
    const uint8_t* texels = texture.pixels;
    const uint16_t* lit = palette.entries;
    constexpr uint32_t kSpecial = MICROPIXEL_RASTER_WARP_ENTRY_SKIP | MICROPIXEL_RASTER_WARP_ENTRY_SOLID;
    auto textured = [&](uint32_t w) -> uint16_t {
        const uint32_t light_row = ((w >> MICROPIXEL_RASTER_WARP_LIGHT_SHIFT) & MICROPIXEL_RASTER_WARP_LIGHT_MASK)
                                   << 8U;
        const uint32_t u = ((w + u_offset) >> u_shift) & u_mask;
        const uint32_t v = ((w >> MICROPIXEL_RASTER_WARP_V_SHIFT) + v_offset) & v_mask;
        return lit[light_row | texels[(v << log2_width) | u]];
    };
    for (uint32_t y = clip.y0; y < clip.y1; ++y) {
        const uint32_t map_row = clip.skip_y + (y - clip.y0);
        // Walk only the row's non-skipped span (intersected with the clip);
        // the skipped remainder is filled or left alone without a read.
        uint32_t begin = clip.skip_x;
        uint32_t end = clip.skip_x + (clip.x1 - clip.x0);
        if (warp.row_spans != nullptr) {
            begin = std::max<uint32_t>(begin, warp.row_spans[map_row * 2U]);
            end = std::min<uint32_t>(end, warp.row_spans[map_row * 2U + 1U]);
            if (end < begin) end = begin;
        }
        uint16_t* row = Row(target, y) + clip.x0 - clip.skip_x;  // indexed by map x
        if (fill) {
            for (uint32_t x = clip.skip_x; x < begin; ++x) row[x] = fill_color;
            for (uint32_t x = end; x < clip.skip_x + (clip.x1 - clip.x0); ++x) row[x] = fill_color;
        }
        const uint32_t* entry = warp.entries + map_row * warp.width;
        uint32_t x = begin;
        while (x < end) {
            // Two textured entries per step keep two gathers in flight on the
            // in-order core; a skip/solid entry is handled singly below.
            if (x + 2U <= end) {
                const uint32_t w0 = entry[x];
                const uint32_t w1 = entry[x + 1U];
                if (((w0 | w1) & kSpecial) == 0U) {
                    const uint16_t c0 = textured(w0);
                    const uint16_t c1 = textured(w1);
                    row[x] = c0;
                    row[x + 1U] = c1;
                    x += 2U;
                    continue;
                }
            }
            const uint32_t w = entry[x];
            if (static_cast<int32_t>(w) < 0) {  // ENTRY_SKIP
                if (fill) row[x] = fill_color;
            } else if ((w & MICROPIXEL_RASTER_WARP_ENTRY_SOLID) != 0U) {
                const uint32_t light_row =
                    ((w >> MICROPIXEL_RASTER_WARP_LIGHT_SHIFT) & MICROPIXEL_RASTER_WARP_LIGHT_MASK) << 8U;
                row[x] = lit[light_row | (w & 0xFFU)];
            } else {
                row[x] = textured(w);
            }
            ++x;
        }
    }
}

// ---- Polygons -------------------------------------------------------------
// Edge-walking scanline rasterizer. Vertex x/y arrive in 12.4; the walker
// keeps x and the attributes (u, v in texels; light in levels) in 16.16 and
// samples at pixel centres: row r covers centre y = r + 0.5, pixel x covers
// centre x + 0.5. Per-edge and per-span gradients are set up with one float
// reciprocal each (the Host cores have an FPU; a 64-bit integer division per
// attribute would cost more than the span it serves); the per-pixel loops are
// integer only. Light is clamped per span to the range the corners span, so a
// rounding drift can never index a palette row the record did not name.
namespace {

constexpr uint32_t kSubpixelShift = 4U;  // vertex x/y are 12.4
constexpr uint32_t kTexelShift = 8U;     // vertex u/v are 8.8
constexpr int32_t kHalfRow = 1 << (kSubpixelShift - 1U);

// First pixel row whose centre lies at or below `y` (12.4): ceil((y - 8) / 16).
[[nodiscard]] constexpr int32_t RowCeil(int32_t y) { return (y + kHalfRow - 1) >> kSubpixelShift; }
// First pixel column whose centre lies at or right of `x` (16.16).
[[nodiscard]] constexpr int32_t ColumnCeil(int32_t x) { return (x + 0x7FFF) >> kFixedShift; }

struct EdgeWalker final {
    int32_t x{};  // 16.16 pixels at the current row centre
    int32_t u{};  // 16.16 texels
    int32_t v{};
    int32_t light{};  // 16.16 levels
    int32_t dx{};     // per row
    int32_t du{};
    int32_t dv{};
    int32_t dlight{};
    int32_t end_row{};  // exclusive

    void Step() {
        x += dx;
        u += du;
        v += dv;
        light += dlight;
    }
};

// Prepares the walk down the edge a -> b (a above b) beginning at `start_row`.
// False when the edge covers no row centre from start_row on.
[[nodiscard]] bool SetupEdge(const micropixel_raster_vertex_t& a, const micropixel_raster_vertex_t& b,
                             int32_t start_row, EdgeWalker& edge) {
    const int32_t dy = static_cast<int32_t>(b.y) - a.y;
    if (dy <= 0) return false;
    const int32_t first_row = std::max(RowCeil(a.y), start_row);
    edge.end_row = RowCeil(b.y);
    if (edge.end_row <= first_row) return false;
    // Per-row steps: dy is in 1/16 rows, so value / (dy / 16) per row.
    const float per_row = 16.0F / static_cast<float>(dy);
    edge.dx = static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(b.x) - a.x) * per_row *
                                   static_cast<float>(1 << (kFixedShift - kSubpixelShift)));
    const int32_t u0 = static_cast<int32_t>(a.u) << (kFixedShift - kTexelShift);
    const int32_t v0 = static_cast<int32_t>(a.v) << (kFixedShift - kTexelShift);
    const int32_t l0 = static_cast<int32_t>(a.light) << kFixedShift;
    edge.du = static_cast<int32_t>(static_cast<float>((static_cast<int32_t>(b.u) << (kFixedShift - kTexelShift)) - u0) *
                                   per_row);
    edge.dv = static_cast<int32_t>(static_cast<float>((static_cast<int32_t>(b.v) << (kFixedShift - kTexelShift)) - v0) *
                                   per_row);
    edge.dlight =
        static_cast<int32_t>(static_cast<float>((static_cast<int32_t>(b.light) << kFixedShift) - l0) * per_row);
    // Values at the first row centre: a + step * (rows from a), rows in 1/16.
    const int64_t rows16 = (static_cast<int64_t>(first_row) << kSubpixelShift) + kHalfRow - a.y;
    const auto at = [rows16](int32_t start, int32_t step) {
        return start + static_cast<int32_t>((static_cast<int64_t>(step) * rows16) >> kSubpixelShift);
    };
    edge.x = at(static_cast<int32_t>(a.x) << (kFixedShift - kSubpixelShift), edge.dx);
    edge.u = at(u0, edge.du);
    edge.v = at(v0, edge.dv);
    edge.light = at(l0, edge.dlight);
    return true;
}

// One side of the polygon: walks its edges from the top vertex downwards.
struct Chain final {
    const micropixel_raster_vertex_t* vertices{};
    uint32_t count{};
    uint32_t index{};      // vertex the current edge starts at
    uint32_t remaining{};  // edges not yet consumed
    bool forward{};
    EdgeWalker edge{};

    // Loads the next edge that covers row `row` or below. False when the
    // chain has no edge left.
    [[nodiscard]] bool Advance(int32_t row) {
        while (remaining > 0U) {
            --remaining;
            const uint32_t next =
                forward ? (index + 1U == count ? 0U : index + 1U) : (index == 0U ? count - 1U : index - 1U);
            const bool covers = SetupEdge(vertices[index], vertices[next], row, edge);
            index = next;
            if (covers) return true;
        }
        return false;
    }
};

struct SpanSetup final {
    uint16_t* row{};
    uint32_t count{};
    int32_t u{}, v{}, light{};
    int32_t du{}, dv{}, dlight{};
};

template <bool kTransparent, bool kGouraud>
void FillTexturedSpan(const SpanSetup& span, const Texture& texture, const uint16_t* palette) {
    const uint8_t* texels = texture.pixels;
    const uint32_t mask_u = static_cast<uint32_t>(texture.width) - 1U;
    const uint32_t mask_v = static_cast<uint32_t>(texture.height) - 1U;
    const uint32_t log2_width = texture.log2_width;
    uint32_t u = static_cast<uint32_t>(span.u);
    uint32_t v = static_cast<uint32_t>(span.v);
    uint32_t light = static_cast<uint32_t>(span.light);
    const uint32_t du = static_cast<uint32_t>(span.du);
    const uint32_t dv = static_cast<uint32_t>(span.dv);
    const uint32_t dlight = static_cast<uint32_t>(span.dlight);
    uint16_t* out = span.row;
    const auto index = [&](uint32_t uu, uint32_t vv) {
        return (((vv >> kFixedShift) & mask_v) << log2_width) | ((uu >> kFixedShift) & mask_u);
    };
    uint32_t i = 0U;
    if constexpr (!kTransparent && !kGouraud) {
        // Four independent texel chains per iteration so the in-order core
        // overlaps the texel load with the palette lookup (as DrawColumn).
        for (; i + 4U <= span.count; i += 4U) {
            const uint8_t t0 = texels[index(u, v)];
            const uint8_t t1 = texels[index(u + du, v + dv)];
            const uint8_t t2 = texels[index(u + 2U * du, v + 2U * dv)];
            const uint8_t t3 = texels[index(u + 3U * du, v + 3U * dv)];
            out[i] = palette[t0];
            out[i + 1U] = palette[t1];
            out[i + 2U] = palette[t2];
            out[i + 3U] = palette[t3];
            u += 4U * du;
            v += 4U * dv;
        }
    }
    for (; i < span.count; ++i) {
        const uint8_t texel = texels[index(u, v)];
        if (!kTransparent || texel != 0U) {
            if constexpr (kGouraud) {
                out[i] = palette[((light >> kFixedShift) << 8U) | texel];
            } else {
                out[i] = palette[texel];
            }
        }
        u += du;
        v += dv;
        light += dlight;
    }
}

void FillFlatSpan(const SpanSetup& span, const uint16_t* palette, uint32_t color_index, bool gouraud) {
    uint16_t* out = span.row;
    if (!gouraud) {
        const uint16_t color = palette[color_index];
        for (uint32_t i = 0U; i < span.count; ++i) out[i] = color;
        return;
    }
    uint32_t light = static_cast<uint32_t>(span.light);
    for (uint32_t i = 0U; i < span.count; ++i, light += static_cast<uint32_t>(span.dlight)) {
        out[i] = palette[((light >> kFixedShift) << 8U) | color_index];
    }
}

}  // namespace

void DrawPolygon(const Target& target, const Texture* texture, const Palette& palette, uint8_t flags,
                 const micropixel_raster_vertex_t* vertices, uint32_t vertex_count) {
    if (vertex_count < 3U || vertex_count > 4U) return;
    const int64_t area2 = PolygonArea2(vertices, vertex_count);
    if (area2 == 0) return;
    const bool flat = (flags & MICROPIXEL_RASTER_POLYGON_FLAT_COLOR) != 0U;
    const bool transparent = (flags & MICROPIXEL_RASTER_POLYGON_TRANSPARENT_INDEX0) != 0U;
    if (!flat && (texture == nullptr || texture->pixels == nullptr)) return;

    uint32_t top = 0U;
    int32_t min_y = vertices[0].y;
    int32_t max_y = vertices[0].y;
    uint32_t min_light = vertices[0].light;
    uint32_t max_light = vertices[0].light;
    for (uint32_t i = 1U; i < vertex_count; ++i) {
        if (vertices[i].y < min_y) {
            min_y = vertices[i].y;
            top = i;
        }
        max_y = std::max<int32_t>(max_y, vertices[i].y);
        min_light = std::min<uint32_t>(min_light, vertices[i].light);
        max_light = std::max<uint32_t>(max_light, vertices[i].light);
    }
    if (max_light >= palette.light_levels) return;  // ValidateDrawList refuses this; keep the kernel safe anyway
    int32_t row = std::max<int32_t>(RowCeil(min_y), 0);
    const int32_t row_end = std::min<int32_t>(RowCeil(max_y), static_cast<int32_t>(target.height));
    if (row >= row_end) return;

    // With y down, positive area is clockwise on screen: walking forward from
    // the top vertex descends the right side.
    Chain left{vertices, vertex_count, top, vertex_count - 1U, area2 < 0, {}};
    Chain right{vertices, vertex_count, top, vertex_count - 1U, area2 > 0, {}};
    if (!left.Advance(row) || !right.Advance(row)) return;

    const int32_t light_low = static_cast<int32_t>(min_light) << kFixedShift;
    const int32_t light_high = static_cast<int32_t>((max_light << kFixedShift) | 0xFFFFU);
    const uint32_t flat_index = flat ? (static_cast<uint32_t>(vertices[0].u) >> kTexelShift) : 0U;
    const int32_t target_width = static_cast<int32_t>(target.width);

    for (;;) {
        const EdgeWalker* l = &left.edge;
        const EdgeWalker* r = &right.edge;
        if (l->x > r->x) std::swap(l, r);
        const int32_t width = r->x - l->x;
        int32_t x0 = ColumnCeil(l->x);
        int32_t x1 = ColumnCeil(r->x);  // exclusive
        x0 = std::max<int32_t>(x0, 0);
        x1 = std::min(x1, target_width);
        if (width > 0 && x1 > x0) {
            SpanSetup span{};
            span.row = Row(target, static_cast<uint32_t>(row)) + x0;
            span.count = static_cast<uint32_t>(x1 - x0);
            const float per_pixel = static_cast<float>(1 << kFixedShift) / static_cast<float>(width);
            span.du = static_cast<int32_t>(static_cast<float>(r->u - l->u) * per_pixel);
            span.dv = static_cast<int32_t>(static_cast<float>(r->v - l->v) * per_pixel);
            span.dlight = static_cast<int32_t>(static_cast<float>(r->light - l->light) * per_pixel);
            // Distance from the left edge to the first pixel centre, 16.16.
            const int64_t prestep = ((static_cast<int64_t>(x0) << kFixedShift) + 0x8000) - l->x;
            const auto at = [prestep](int32_t start, int32_t step) {
                return start + static_cast<int32_t>((static_cast<int64_t>(step) * prestep) >> kFixedShift);
            };
            span.u = at(l->u, span.du);
            span.v = at(l->v, span.dv);
            span.light = std::clamp(at(l->light, span.dlight), light_low, light_high);
            const int32_t last = static_cast<int32_t>(span.count) - 1;
            int32_t light_end = span.light + static_cast<int32_t>(static_cast<int64_t>(span.dlight) * last);
            if (light_end < light_low || light_end > light_high) {
                light_end = std::clamp(light_end, light_low, light_high);
                span.dlight = last > 0 ? (light_end - span.light) / last : 0;
            }
            const bool gouraud = (span.light >> kFixedShift) != (light_end >> kFixedShift);
            if (flat) {
                FillFlatSpan(span,
                             gouraud ? palette.entries : palette.Row(static_cast<uint32_t>(span.light >> kFixedShift)),
                             flat_index, gouraud);
            } else if (gouraud) {
                if (transparent) {
                    FillTexturedSpan<true, true>(span, *texture, palette.entries);
                } else {
                    FillTexturedSpan<false, true>(span, *texture, palette.entries);
                }
            } else {
                const uint16_t* lit = palette.Row(static_cast<uint32_t>(span.light >> kFixedShift));
                if (transparent) {
                    FillTexturedSpan<true, false>(span, *texture, lit);
                } else {
                    FillTexturedSpan<false, false>(span, *texture, lit);
                }
            }
        }
        if (++row >= row_end) break;
        if (row == left.edge.end_row) {
            if (!left.Advance(row)) break;
        } else {
            left.edge.Step();
        }
        if (row == right.edge.end_row) {
            if (!right.Advance(row)) break;
        } else {
            right.edge.Step();
        }
    }
}

namespace {

// An IMAGE the engine can take: fully opaque, unscaled RGB565 whose texture is
// stored in the target's byte order, with a clipped area worth the setup cost.
[[nodiscard]] bool CopyEligible(const Target& target, const device::BitmapView& texture,
                                const micropixel_raster_image_t& image, device::OpaqueCopyBlock& block_out) {
    if (image.opacity != 255U || texture.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565 ||
        image.source_width != image.width || image.source_height != image.height ||
        ((texture.flags & device::bitmap_flags::kRgb565ByteSwapped) != 0U) != target.byte_swapped) {
        return false;
    }
    ClippedRect clipped{};
    if (!ClipRect(target, image.x, image.y, image.width, image.height, clipped)) return false;
    const uint32_t width = clipped.x1 - clipped.x0;
    const uint32_t height = clipped.y1 - clipped.y0;
    if (width < kMinCopyBlockWidth || static_cast<uint64_t>(width) * height < kMinCopyBlockPixels) return false;
    block_out = device::OpaqueCopyBlock{
        .source_pixels = texture.data,
        .source_stride = texture.stride,
        .source_picture_width = texture.width,
        .source_picture_height = texture.height,
        .source_x = image.source_x + clipped.skip_x,
        .source_y = image.source_y + clipped.skip_y,
        .destination_x = clipped.x0,
        .destination_y = clipped.y0,
        .width = width,
        .height = height,
    };
    return true;
}

void FlushCopies(const Target& target, const Resources& resources, CopyBatch& pending, ExecuteProfile* profile) {
    if (pending.count == 0U) return;
    const uint64_t started_us = profile != nullptr && profile->now_us != nullptr ? profile->now_us() : 0U;
    const bool copied = resources.copy_blocks(resources.copy_context, target, pending.blocks, pending.count);
    if (!copied) {
        // The engine may have written some blocks; redrawing all of them in
        // order restores exactly what the records asked for.
        for (uint32_t index = 0U; index < pending.count; ++index) {
            DrawImage(target, pending.textures[index], pending.images[index]);
        }
    }
    if (profile != nullptr) {
        const uint64_t elapsed_us = profile->now_us != nullptr ? profile->now_us() - started_us : 0U;
        profile->time_us[MICROPIXEL_RASTER_RECORD_IMAGE] += elapsed_us;
        profile->copy_time_us += elapsed_us;
        ++profile->copy_batches;
        if (copied) {
            profile->copy_blocks += pending.count;
            for (uint32_t index = 0U; index < pending.count; ++index) {
                profile->copy_pixels +=
                    static_cast<uint64_t>(pending.blocks[index].width) * pending.blocks[index].height;
            }
        } else {
            ++profile->copy_failures;
        }
    }
    pending.count = 0U;
}

}  // namespace

void ExecuteDrawList(const uint8_t* bytes, const micropixel_raster_header_t& header, const Target& target,
                     const Resources& resources, ExecuteProfile* profile) {
    uint32_t offset = sizeof(micropixel_raster_header_t);
    const bool copy_engine = resources.copy_blocks != nullptr && resources.copy_batch != nullptr;
    CopyBatch* pending = copy_engine ? resources.copy_batch : nullptr;
    if (copy_engine) pending->count = 0U;
    for (uint32_t index = 0U; index < header.record_count; ++index) {
        const uint8_t type = bytes[offset];
        uint64_t pixels = 0U;
        // Anything that is not another eligible IMAGE draws after the queued
        // copies, so overlapping records keep their order.
        if (copy_engine && type != MICROPIXEL_RASTER_RECORD_IMAGE) FlushCopies(target, resources, *pending, profile);
        const uint64_t started_us = profile != nullptr && profile->now_us != nullptr ? profile->now_us() : 0U;
        if (type == MICROPIXEL_RASTER_RECORD_COLUMN) {
            micropixel_raster_column_t column{};
            std::memcpy(&column, bytes + offset, sizeof(column));
            offset += sizeof(column);
            DrawColumn(target, *resources.TextureAt(column.texture_slot),
                       resources.PaletteAt(column.palette_slot)->Row(column.light_level), column);
            pixels = column.y1 >= column.y0 ? static_cast<uint64_t>(column.y1 - column.y0 + 1) : 0U;
        } else if (type == MICROPIXEL_RASTER_RECORD_SPAN_PAIR) {
            micropixel_raster_span_pair_t span{};
            std::memcpy(&span, bytes + offset, sizeof(span));
            offset += sizeof(span);
            DrawSpanPair(target, *resources.TextureAt(span.floor_texture_slot),
                         *resources.TextureAt(span.ceiling_texture_slot),
                         resources.PaletteAt(span.palette_slot)->Row(span.light_level), span);
            pixels = span.x1 >= span.x0 ? static_cast<uint64_t>(span.x1 - span.x0 + 1) * 2U : 0U;
        } else if (type == MICROPIXEL_RASTER_RECORD_SPAN) {
            micropixel_raster_span_t span{};
            std::memcpy(&span, bytes + offset, sizeof(span));
            offset += sizeof(span);
            DrawSpan(target, *resources.TextureAt(span.texture_slot),
                     resources.PaletteAt(span.palette_slot)->Row(span.light_level), span);
            pixels = span.x1 >= span.x0 ? static_cast<uint64_t>(span.x1 - span.x0 + 1) : 0U;
        } else if (type == MICROPIXEL_RASTER_RECORD_TEXT) {
            micropixel_raster_text_t text{};
            std::memcpy(&text, bytes + offset, sizeof(text));
            const auto* payload = reinterpret_cast<const char*>(bytes + offset + sizeof(text));
            offset += sizeof(text) + PaddedTextBytes(text.text_length);
            // Validation confirmed the font and the text; a draw failure here
            // (for example the font released mid-list) leaves the pixels alone.
            (void)resources.draw_text(resources.text_context, target, text.x, text.y, text.color, text.font_handle,
                                      payload, text.text_length);
            pixels = text.text_length;
        } else if (type == MICROPIXEL_RASTER_RECORD_SPRITE) {
            micropixel_raster_sprite_t sprite{};
            std::memcpy(&sprite, bytes + offset, sizeof(sprite));
            offset += sizeof(sprite);
            // SOLID_COLOR sprites validated without a palette; hand them a
            // null row they never read.
            const uint16_t* lit = (sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) != 0U
                                      ? nullptr
                                      : resources.PaletteAt(sprite.palette_slot)->Row(sprite.light_level);
            DrawSprite(target, *resources.TextureAt(sprite.texture_slot), lit, sprite);
            pixels = static_cast<uint64_t>(sprite.width) * sprite.height;
        } else if (type == MICROPIXEL_RASTER_RECORD_WARP) {
            micropixel_raster_warp_t warp{};
            std::memcpy(&warp, bytes + offset, sizeof(warp));
            offset += sizeof(warp);
            const WarpMap& map = *resources.WarpAt(warp.warp_slot);
            DrawWarp(target, map, *resources.TextureAt(warp.texture_slot), *resources.PaletteAt(warp.palette_slot),
                     warp);
            pixels = static_cast<uint64_t>(map.width) * map.height;
        } else if (type == MICROPIXEL_RASTER_RECORD_IMAGE) {
            micropixel_raster_image_t image{};
            std::memcpy(&image, bytes + offset, sizeof(image));
            offset += sizeof(image);
            device::BitmapView texture{};
            if (resources.resolve_texture(resources.texture_context, image.texture_handle, texture)) {
                device::OpaqueCopyBlock block{};
                if (copy_engine && CopyEligible(target, texture, image, block)) {
                    pending->blocks[pending->count] = block;
                    pending->images[pending->count] = image;
                    pending->textures[pending->count] = texture;
                    if (++pending->count == kMaxCopyBlocks) FlushCopies(target, resources, *pending, profile);
                } else {
                    if (copy_engine) FlushCopies(target, resources, *pending, profile);
                    DrawImage(target, texture, image);
                }
            }
            pixels = static_cast<uint64_t>(image.width) * image.height;
        } else if (type == MICROPIXEL_RASTER_RECORD_TRIANGLE) {
            micropixel_raster_triangle_t triangle{};
            std::memcpy(&triangle, bytes + offset, sizeof(triangle));
            offset += sizeof(triangle);
            const Texture* texture = (triangle.flags & MICROPIXEL_RASTER_POLYGON_FLAT_COLOR) != 0U
                                         ? nullptr
                                         : resources.TextureAt(triangle.texture_slot);
            DrawPolygon(target, texture, *resources.PaletteAt(triangle.palette_slot), triangle.flags, triangle.vertices,
                        3U);
            pixels = static_cast<uint64_t>(std::abs(PolygonArea2(triangle.vertices, 3U))) / 512U;
        } else if (type == MICROPIXEL_RASTER_RECORD_QUAD) {
            micropixel_raster_quad_t quad{};
            std::memcpy(&quad, bytes + offset, sizeof(quad));
            offset += sizeof(quad);
            const Texture* texture = (quad.flags & MICROPIXEL_RASTER_POLYGON_FLAT_COLOR) != 0U
                                         ? nullptr
                                         : resources.TextureAt(quad.texture_slot);
            DrawPolygon(target, texture, *resources.PaletteAt(quad.palette_slot), quad.flags, quad.vertices, 4U);
            pixels = static_cast<uint64_t>(std::abs(PolygonArea2(quad.vertices, 4U))) / 512U;
        } else {
            micropixel_raster_rect_t rect{};
            std::memcpy(&rect, bytes + offset, sizeof(rect));
            offset += sizeof(rect);
            DrawRect(target, rect);
            pixels = static_cast<uint64_t>(rect.width) * rect.height;
        }
        if (profile != nullptr) {
            const uint32_t kind = type < ExecuteProfile::kKinds ? type : MICROPIXEL_RASTER_RECORD_RECT;
            ++profile->records[kind];
            profile->pixels[kind] += pixels;
            if (profile->now_us != nullptr) {
                profile->time_us[kind] += profile->now_us() - started_us;
            }
        }
    }
    if (copy_engine) FlushCopies(target, resources, *pending, profile);
}

}  // namespace micropixel::runtime::raster
