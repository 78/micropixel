#include "runtime/graphics/raster_kernels.hpp"

#include <cstring>

namespace micropixel::runtime::raster {
namespace {

constexpr uint32_t kBytesPerRgb565 = 2U;
constexpr uint32_t kFixedShift = 16U;

[[nodiscard]] uint32_t RecordSize(uint8_t type) {
    switch (type) {
        case MICROPIXEL_RASTER_RECORD_COLUMN:
            return sizeof(micropixel_raster_column_t);
        case MICROPIXEL_RASTER_RECORD_SPAN_PAIR:
            return sizeof(micropixel_raster_span_pair_t);
        case MICROPIXEL_RASTER_RECORD_SPRITE:
            return sizeof(micropixel_raster_sprite_t);
        case MICROPIXEL_RASTER_RECORD_RECT:
            return sizeof(micropixel_raster_rect_t);
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

// Blends `color` over `dst` in canonical RGB565 with `alpha` in 0..256.
[[nodiscard]] inline uint16_t Blend565(uint16_t dst, uint16_t color, uint32_t alpha) {
    const uint32_t inverse = 256U - alpha;
    const uint32_t r = ((dst >> 11U) * inverse + (color >> 11U) * alpha) >> 8U;
    const uint32_t g = (((dst >> 5U) & 0x3FU) * inverse + ((color >> 5U) & 0x3FU) * alpha) >> 8U;
    const uint32_t b = ((dst & 0x1FU) * inverse + (color & 0x1FU) * alpha) >> 8U;
    return static_cast<uint16_t>((r << 11U) | (g << 5U) | b);
}

[[nodiscard]] const Texture* SlotWithLayout(const Resources& resources, uint32_t slot, uint8_t layout) {
    if (slot >= resources.texture_count) {
        return nullptr;
    }
    const Texture& texture = resources.textures[slot];
    if (texture.pixels == nullptr || texture.layout != layout) {
        return nullptr;
    }
    return &texture;
}

[[nodiscard]] int32_t ValidateColumn(const micropixel_raster_column_t& column, const micropixel_raster_header_t& header,
                                     const Resources& resources) {
    if ((column.flags & ~MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0) != 0U || column.reserved0 != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (column.x >= header.target_width || column.y0 < 0 || column.y1 < column.y0 ||
        column.y1 >= static_cast<int32_t>(header.target_height)) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const Texture* texture = SlotWithLayout(resources, column.texture, MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR);
    if (texture == nullptr || column.u >= texture->width) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (column.light >= resources.palette.light_levels) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateSpanPair(const micropixel_raster_span_pair_t& span,
                                       const micropixel_raster_header_t& header, const Resources& resources) {
    if (span.flags != 0U || span.reserved0[0] != 0U || span.reserved0[1] != 0U || span.reserved0[2] != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (span.y_floor >= header.target_height || span.y_ceiling >= header.target_height || span.x1 < span.x0 ||
        span.x1 >= header.target_width) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (SlotWithLayout(resources, span.floor_texture, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR) == nullptr ||
        SlotWithLayout(resources, span.ceiling_texture, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR) == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (span.light >= resources.palette.light_levels) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateSprite(const micropixel_raster_sprite_t& sprite, const Resources& resources) {
    if ((sprite.flags & ~(MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 | MICROPIXEL_RASTER_SPRITE_SOLID_COLOR)) != 0U ||
        sprite.reserved0 != 0U || sprite.width == 0U || sprite.height == 0U || sprite.src_width == 0U ||
        sprite.src_height == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const Texture* texture = SlotWithLayout(resources, sprite.texture, MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR);
    if (texture == nullptr) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (static_cast<uint32_t>(sprite.u0) + sprite.src_width > texture->width ||
        static_cast<uint32_t>(sprite.v0) + sprite.src_height > texture->height) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if ((sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) == 0U && sprite.light >= resources.palette.light_levels) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] int32_t ValidateRect(const micropixel_raster_rect_t& rect) {
    if (rect.flags != 0U || rect.reserved0 != 0U || rect.reserved1 != 0U || rect.alpha == 0U || rect.width == 0U ||
        rect.height == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    return MICROPIXEL_STATUS_OK;
}

[[nodiscard]] inline uint16_t* Row(const Target& target, uint32_t y) {
    return reinterpret_cast<uint16_t*>(target.pixels + y * target.pitch);
}

}  // namespace

bool ValidTextureDimension(uint32_t value) {
    return value >= MICROPIXEL_GRAPHICS_RASTER_MIN_TEXTURE_SIZE &&
           value <= MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURE_SIZE && (value & (value - 1U)) == 0U;
}

uint8_t Log2Exact(uint32_t power_of_two) {
    uint8_t result = 0U;
    while (power_of_two > 1U) {
        power_of_two >>= 1U;
        ++result;
    }
    return result;
}

int32_t ValidateDrawList(const uint8_t* bytes, uint32_t length, const Resources& resources,
                         micropixel_raster_header_t& header_out) {
    if (bytes == nullptr || length < sizeof(micropixel_raster_header_t) ||
        length > MICROPIXEL_GRAPHICS_MAX_RASTER_BYTES) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    micropixel_raster_header_t header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != MICROPIXEL_GRAPHICS_RASTER_MAGIC ||
        header.interface_major != MICROPIXEL_GRAPHICS_INTERFACE_MAJOR ||
        header.interface_minor > MICROPIXEL_GRAPHICS_INTERFACE_MINOR || header.total_size != length ||
        header.flags != 0U || header.reserved0 != 0U || header.record_count == 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (header.target_width == 0U || header.target_height == 0U || (header.target_pitch % kBytesPerRgb565) != 0U ||
        header.target_pitch < static_cast<uint32_t>(header.target_width) * kBytesPerRgb565) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    // RECT and SOLID_COLOR sprites need no palette; every other record does,
    // and a missing palette is reported before the record is looked at.
    const bool palette_ready = resources.palette.entries != nullptr && resources.palette.light_levels != 0U;

    uint32_t offset = sizeof(header);
    for (uint32_t index = 0U; index < header.record_count; ++index) {
        if (offset >= length) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        const uint8_t type = bytes[offset];  // every record starts with its type byte
        const uint32_t record_size = RecordSize(type);
        if (record_size == 0U || record_size > length - offset) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        int32_t status = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        if (type == MICROPIXEL_RASTER_RECORD_COLUMN) {
            if (!palette_ready) {
                return MICROPIXEL_STATUS_STALE_STATE;
            }
            micropixel_raster_column_t column{};
            std::memcpy(&column, bytes + offset, sizeof(column));
            status = ValidateColumn(column, header, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_SPAN_PAIR) {
            if (!palette_ready) {
                return MICROPIXEL_STATUS_STALE_STATE;
            }
            micropixel_raster_span_pair_t span{};
            std::memcpy(&span, bytes + offset, sizeof(span));
            status = ValidateSpanPair(span, header, resources);
        } else if (type == MICROPIXEL_RASTER_RECORD_SPRITE) {
            micropixel_raster_sprite_t sprite{};
            std::memcpy(&sprite, bytes + offset, sizeof(sprite));
            if (!palette_ready && (sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) == 0U) {
                return MICROPIXEL_STATUS_STALE_STATE;
            }
            status = ValidateSprite(sprite, resources);
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

void DrawSprite(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_sprite_t& sprite) {
    ClippedRect clip{};
    if (!ClipRect(target, sprite.x, sprite.y, sprite.width, sprite.height, clip)) {
        return;
    }
    const bool transparent = (sprite.flags & MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0) != 0U;
    const bool solid = (sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) != 0U;
    const uint16_t solid_color = target.byte_swapped ? ByteSwap(sprite.color) : sprite.color;
    // 16.16 texel steps per destination pixel; sampling starts at the centre
    // of the first visible pixel so a clipped sprite keeps its phase.
    const uint32_t u_step = (static_cast<uint32_t>(sprite.src_width) << kFixedShift) / sprite.width;
    const uint32_t v_step = (static_cast<uint32_t>(sprite.src_height) << kFixedShift) / sprite.height;
    uint32_t v = clip.skip_y * v_step + (v_step >> 1U);
    const uint32_t u_start = clip.skip_x * u_step + (u_step >> 1U);
    const uint32_t texture_height = texture.height;
    for (uint32_t y = clip.y0; y < clip.y1; ++y, v += v_step) {
        const uint8_t* texels = texture.pixels + (static_cast<uint32_t>(sprite.v0) + (v >> kFixedShift));
        uint16_t* row = Row(target, y) + clip.x0;
        uint32_t u = u_start;
        for (uint32_t x = clip.x0; x < clip.x1; ++x, u += u_step) {
            const uint8_t texel = texels[(static_cast<uint32_t>(sprite.u0) + (u >> kFixedShift)) * texture_height];
            if (transparent && texel == 0U) {
                continue;
            }
            row[x - clip.x0] = solid ? solid_color : lit[texel];
        }
    }
}

void DrawRect(const Target& target, const micropixel_raster_rect_t& rect) {
    ClippedRect clip{};
    if (!ClipRect(target, rect.x, rect.y, rect.width, rect.height, clip)) {
        return;
    }
    const uint32_t count = clip.x1 - clip.x0;
    if (rect.alpha == 0xFFU) {
        const uint16_t color = target.byte_swapped ? ByteSwap(rect.color) : rect.color;
        for (uint32_t y = clip.y0; y < clip.y1; ++y) {
            uint16_t* row = Row(target, y) + clip.x0;
            for (uint32_t x = 0U; x < count; ++x) {
                row[x] = color;
            }
        }
        return;
    }
    // Blend in canonical order; swapped targets are converted per pixel.
    const uint32_t alpha = static_cast<uint32_t>(rect.alpha) + 1U;  // 1..254 -> 2..255 (255 handled above)
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
    for (int32_t index = 0; index < count; ++index) {
        *reinterpret_cast<uint16_t*>(row) = lit[texels[(v >> kFixedShift) & mask]];
        row += pitch;
        v += step;
    }
}

void DrawSpanPair(const Target& target, const Texture& floor_texture, const Texture& ceiling_texture,
                  const uint16_t* lit, const micropixel_raster_span_pair_t& span) {
    uint16_t* floor = Row(target, span.y_floor) + span.x0;
    uint16_t* ceiling = Row(target, span.y_ceiling) + span.x0;
    const uint32_t count = static_cast<uint32_t>(span.x1) - span.x0 + 1U;
    uint32_t s = static_cast<uint32_t>(span.s);
    uint32_t t = static_cast<uint32_t>(span.t);
    const uint32_t ds = static_cast<uint32_t>(span.ds);
    const uint32_t dt = static_cast<uint32_t>(span.dt);
    if (floor_texture.width == ceiling_texture.width && floor_texture.height == ceiling_texture.height) {
        // Common case: one texel index serves both rows.
        const uint32_t shift_s = kFixedShift - floor_texture.log2_width;
        const uint32_t shift_t = kFixedShift - floor_texture.log2_height;
        const uint32_t mask_x = static_cast<uint32_t>(floor_texture.width) - 1U;
        const uint32_t mask_y = static_cast<uint32_t>(floor_texture.height) - 1U;
        const uint32_t log2_width = floor_texture.log2_width;
        const uint8_t* floor_texels = floor_texture.pixels;
        const uint8_t* ceiling_texels = ceiling_texture.pixels;
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t texel = (((t >> shift_t) & mask_y) << log2_width) | ((s >> shift_s) & mask_x);
            floor[index] = lit[floor_texels[texel]];
            ceiling[index] = lit[ceiling_texels[texel]];
            s += ds;
            t += dt;
        }
        return;
    }
    const uint32_t floor_shift_s = kFixedShift - floor_texture.log2_width;
    const uint32_t floor_shift_t = kFixedShift - floor_texture.log2_height;
    const uint32_t ceiling_shift_s = kFixedShift - ceiling_texture.log2_width;
    const uint32_t ceiling_shift_t = kFixedShift - ceiling_texture.log2_height;
    for (uint32_t index = 0U; index < count; ++index) {
        const uint32_t floor_texel =
            (((t >> floor_shift_t) & (floor_texture.height - 1U)) << floor_texture.log2_width) |
            ((s >> floor_shift_s) & (floor_texture.width - 1U));
        const uint32_t ceiling_texel =
            (((t >> ceiling_shift_t) & (ceiling_texture.height - 1U)) << ceiling_texture.log2_width) |
            ((s >> ceiling_shift_s) & (ceiling_texture.width - 1U));
        floor[index] = lit[floor_texture.pixels[floor_texel]];
        ceiling[index] = lit[ceiling_texture.pixels[ceiling_texel]];
        s += ds;
        t += dt;
    }
}

void ExecuteDrawList(const uint8_t* bytes, const micropixel_raster_header_t& header, const Target& target,
                     const Resources& resources) {
    const uint16_t* palette = resources.palette.entries;
    uint32_t offset = sizeof(micropixel_raster_header_t);
    for (uint32_t index = 0U; index < header.record_count; ++index) {
        const uint8_t type = bytes[offset];
        if (type == MICROPIXEL_RASTER_RECORD_COLUMN) {
            micropixel_raster_column_t column{};
            std::memcpy(&column, bytes + offset, sizeof(column));
            offset += sizeof(column);
            DrawColumn(target, resources.textures[column.texture],
                       palette + static_cast<uint32_t>(column.light) * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES,
                       column);
            continue;
        }
        if (type == MICROPIXEL_RASTER_RECORD_SPAN_PAIR) {
            micropixel_raster_span_pair_t span{};
            std::memcpy(&span, bytes + offset, sizeof(span));
            offset += sizeof(span);
            DrawSpanPair(target, resources.textures[span.floor_texture], resources.textures[span.ceiling_texture],
                         palette + static_cast<uint32_t>(span.light) * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES,
                         span);
            continue;
        }
        if (type == MICROPIXEL_RASTER_RECORD_SPRITE) {
            micropixel_raster_sprite_t sprite{};
            std::memcpy(&sprite, bytes + offset, sizeof(sprite));
            offset += sizeof(sprite);
            // SOLID_COLOR sprites validated without a palette; hand them a
            // null row they never read.
            const uint16_t* lit =
                (sprite.flags & MICROPIXEL_RASTER_SPRITE_SOLID_COLOR) != 0U
                    ? nullptr
                    : palette + static_cast<uint32_t>(sprite.light) * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES;
            DrawSprite(target, resources.textures[sprite.texture], lit, sprite);
            continue;
        }
        micropixel_raster_rect_t rect{};
        std::memcpy(&rect, bytes + offset, sizeof(rect));
        offset += sizeof(rect);
        DrawRect(target, rect);
    }
}

}  // namespace micropixel::runtime::raster
