// Graphics 1.6 raster kernels on the raw ABI: texture and palette uploads, a
// COLUMN, SPAN_PAIR, SPRITE and RECT list accepted into a Host-owned Direct
// Surface buffer (pixel output is verified by the Host unit test; the Guest
// never sees Host buffers), then every malformed request refused with the
// documented status. Exit codes 50..99 name the failed step; 0 also when the
// Host has no raster pool.
#include <stdint.h>

#include "abi/micropixel_abi.h"

namespace {

constexpr uint32_t kTexSize = 16U;
constexpr uint32_t kLightLevels = 2U;

// Panel geometry, filled from GET_INFO / SURFACE_CREATE.
uint32_t g_width;
uint32_t g_height;
micropixel_surface_handle_t g_surface;
uint32_t g_target_buffer;

uint8_t g_wall[kTexSize * kTexSize];   // column-major: texel(u, v) = u * 16 + v
uint8_t g_floor[kTexSize * kTexSize];  // row-major: texel(u, v) = v * 16 + u
uint16_t g_palette[kLightLevels * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES];
alignas(4) uint8_t g_list[sizeof(micropixel_raster_header_t) + sizeof(micropixel_raster_column_t) +
                          sizeof(micropixel_raster_span_pair_t) + sizeof(micropixel_raster_sprite_t) +
                          sizeof(micropixel_raster_rect_t)];

uint32_t g_service_handle;

int32_t Call(uint32_t method, const void* request, uint32_t request_size, void* response = nullptr,
             uint32_t capacity = 0U) {
    uint32_t response_size = 0U;
    return micropixel_service_call(g_service_handle, method, static_cast<const uint8_t*>(request), request_size,
                                   static_cast<uint8_t*>(response), capacity, &response_size);
}

uint32_t GuestAddress(const void* pointer) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer)); }

void Copy(uint8_t* destination, const void* source, uint32_t bytes) {
    const auto* from = static_cast<const uint8_t*>(source);
    for (uint32_t index = 0U; index < bytes; ++index) {
        destination[index] = from[index];
    }
}

micropixel_raster_texture_upload_request_t TextureUpload(uint16_t slot, uint16_t layout, const uint8_t* pixels) {
    micropixel_raster_texture_upload_request_t request{};
    request.size = sizeof(request);
    request.slot = slot;
    request.width = kTexSize;
    request.height = kTexSize;
    request.layout = layout;
    request.pixels = GuestAddress(pixels);
    request.length = kTexSize * kTexSize;
    return request;
}

micropixel_raster_palette_upload_request_t PaletteUpload() {
    micropixel_raster_palette_upload_request_t request{};
    request.size = sizeof(request);
    request.light_levels = kLightLevels;
    request.pixels = GuestAddress(g_palette);
    request.length = kLightLevels * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES * 2U;
    return request;
}

micropixel_raster_header_t Header(uint16_t record_count, uint32_t total_size) {
    micropixel_raster_header_t header{};
    header.magic = MICROPIXEL_GRAPHICS_RASTER_MAGIC;
    header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    header.total_size = total_size;
    header.target_buffer = g_target_buffer;
    header.target_width = static_cast<uint16_t>(g_width);
    header.target_height = static_cast<uint16_t>(g_height);
    header.target_pitch = static_cast<uint16_t>(g_width * 2U);
    header.record_count = record_count;
    return header;
}

micropixel_raster_column_t Column() {
    micropixel_raster_column_t column{};
    column.type = MICROPIXEL_RASTER_RECORD_COLUMN;
    column.texture = 0U;
    column.light = 1U;
    column.x = 5U;
    column.y0 = 2U;
    column.y1 = 17U;  // 16 pixels: one texel per pixel
    column.u = 3U;
    column.v_start = 0;
    column.v_step = 1 << 16;
    return column;
}

micropixel_raster_span_pair_t SpanPair() {
    micropixel_raster_span_pair_t span{};
    span.type = MICROPIXEL_RASTER_RECORD_SPAN_PAIR;
    span.floor_texture = 1U;
    span.ceiling_texture = 1U;
    span.y_floor = 20U;
    span.y_ceiling = 1U;
    span.x0 = 8U;
    span.x1 = 23U;  // 16 pixels: one texel per pixel along u
    span.light = 0U;
    span.s = 0;
    span.t = 2 << 12;  // texel row 2 (16-texel texture: fraction bits 12..15)
    span.ds = 1 << 12;
    span.dt = 0;
    return span;
}

micropixel_raster_sprite_t Sprite() {
    micropixel_raster_sprite_t sprite{};
    sprite.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    sprite.flags = MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0;
    sprite.texture = 0U;
    sprite.light = 1U;
    sprite.x = -4;  // partly off the left edge: the Host clips
    sprite.y = 24;
    sprite.width = 32U;
    sprite.height = 32U;
    sprite.u0 = 0U;
    sprite.v0 = 0U;
    sprite.src_width = kTexSize;
    sprite.src_height = kTexSize;
    return sprite;
}

micropixel_raster_rect_t Rect() {
    micropixel_raster_rect_t rect{};
    rect.type = MICROPIXEL_RASTER_RECORD_RECT;
    rect.alpha = 128U;
    rect.x = 0;
    rect.y = 0;
    rect.width = 0xFFFFU;  // wider than any panel: clipped
    rect.height = 8U;
    rect.color = 0xF800U;
    return rect;
}

// Serializes header + records; returns the list length.
uint32_t BuildList(const micropixel_raster_column_t* column, const micropixel_raster_span_pair_t* span,
                   const micropixel_raster_sprite_t* sprite = nullptr, const micropixel_raster_rect_t* rect = nullptr) {
    uint32_t offset = sizeof(micropixel_raster_header_t);
    uint16_t records = 0U;
    if (column != nullptr) {
        Copy(g_list + offset, column, sizeof(*column));
        offset += sizeof(*column);
        ++records;
    }
    if (span != nullptr) {
        Copy(g_list + offset, span, sizeof(*span));
        offset += sizeof(*span);
        ++records;
    }
    if (sprite != nullptr) {
        Copy(g_list + offset, sprite, sizeof(*sprite));
        offset += sizeof(*sprite);
        ++records;
    }
    if (rect != nullptr) {
        Copy(g_list + offset, rect, sizeof(*rect));
        offset += sizeof(*rect);
        ++records;
    }
    const micropixel_raster_header_t header = Header(records, offset);
    Copy(g_list, &header, sizeof(header));
    return offset;
}

int32_t Submit(uint32_t length) {
    return micropixel_service_submit(g_service_handle, MICROPIXEL_GRAPHICS_CHANNEL_RASTER, g_list, length);
}

int32_t SubmitWithHeader(const micropixel_raster_header_t& header, uint32_t length) {
    Copy(g_list, &header, sizeof(header));
    return Submit(length);
}

}  // namespace

int main() {
    micropixel_service_info_t service{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &service, sizeof(service)) != MICROPIXEL_STATUS_OK) {
        return 50;
    }
    g_service_handle = service.handle;

    micropixel_graphics_info_t info{};
    uint32_t info_size = 0U;
    if (micropixel_service_call(g_service_handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U,
                                reinterpret_cast<uint8_t*>(&info), sizeof(info), &info_size) != MICROPIXEL_STATUS_OK ||
        info.size < sizeof(info)) {
        return 51;
    }
    const bool advertised = (service.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER) != 0U;
    if (advertised != (info.raster_pool_bytes != 0U)) {
        return 52;
    }
    if (!advertised) {
        // Pool disabled by Kconfig: every raster method must fail cleanly.
        const micropixel_raster_palette_upload_request_t palette = PaletteUpload();
        if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD, &palette, sizeof(palette)) !=
            MICROPIXEL_STATUS_UNSUPPORTED) {
            return 53;
        }
        return 0;
    }
    if (info.raster_max_textures != MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES ||
        info.raster_max_light_levels != MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS) {
        return 54;
    }
    g_width = info.width;
    g_height = info.height;

    // Procedural resources: texel value encodes its coordinates.
    for (uint32_t u = 0U; u < kTexSize; ++u) {
        for (uint32_t v = 0U; v < kTexSize; ++v) {
            g_wall[u * kTexSize + v] = static_cast<uint8_t>(u * kTexSize + v);
            g_floor[v * kTexSize + u] = static_cast<uint8_t>(v * kTexSize + u);
        }
    }
    for (uint32_t light = 0U; light < kLightLevels; ++light) {
        for (uint32_t index = 0U; index < MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES; ++index) {
            g_palette[light * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES + index] =
                static_cast<uint16_t>((light << 12U) | index);
        }
    }

    // A draw list before any palette exists is stale state, not a trap.
    const micropixel_raster_column_t column = Column();
    const micropixel_raster_span_pair_t span = SpanPair();
    const micropixel_raster_sprite_t sprite = Sprite();
    const micropixel_raster_rect_t rect = Rect();
    micropixel_raster_texture_upload_request_t wall = TextureUpload(0U, MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR, g_wall);
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &wall, sizeof(wall)) != MICROPIXEL_STATUS_OK) {
        return 55;
    }
    if (Submit(BuildList(&column, nullptr)) != MICROPIXEL_STATUS_STALE_STATE) {
        return 56;
    }
    // A RECT needs no palette, but without a Host-buffer surface there is no
    // target.
    if (Submit(BuildList(nullptr, nullptr, nullptr, &rect)) != MICROPIXEL_STATUS_NOT_FOUND) {
        return 82;
    }
    micropixel_surface_create_request_t create{};
    create.size = sizeof(create);
    create.width = g_width;
    create.height = g_height;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    create.buffer_count = 2U;
    micropixel_surface_create_response_t created{};
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &create, sizeof(create), &created, sizeof(created)) !=
            MICROPIXEL_STATUS_OK ||
        created.surface == 0U) {
        return 83;
    }
    g_surface = created.surface;
    if (Submit(BuildList(nullptr, nullptr, nullptr, &rect)) != MICROPIXEL_STATUS_OK) {
        return 84;
    }

    // Texture upload negatives.
    micropixel_raster_texture_upload_request_t bad = wall;
    bad.slot = MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &bad, sizeof(bad)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 57;
    }
    bad = wall;
    bad.width = 12U;  // not a power of two
    bad.length = 12U * kTexSize;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &bad, sizeof(bad)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 58;
    }
    bad = wall;
    bad.layout = 3U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &bad, sizeof(bad)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 59;
    }
    bad = wall;
    bad.length -= 1U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &bad, sizeof(bad)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 60;
    }
    bad = wall;
    bad.pixels = 0xffff0000U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &bad, sizeof(bad)) != MICROPIXEL_STATUS_INVALID_MEMORY) {
        return 61;
    }

    // Palette upload negatives, then the real one.
    micropixel_raster_palette_upload_request_t palette = PaletteUpload();
    palette.light_levels = 0U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD, &palette, sizeof(palette)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 62;
    }
    palette = PaletteUpload();
    palette.length += 2U;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD, &palette, sizeof(palette)) !=
        MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 63;
    }
    palette = PaletteUpload();
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD, &palette, sizeof(palette)) != MICROPIXEL_STATUS_OK) {
        return 64;
    }
    const micropixel_raster_texture_upload_request_t floor =
        TextureUpload(1U, MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR, g_floor);
    if (Call(MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &floor, sizeof(floor)) != MICROPIXEL_STATUS_OK) {
        return 65;
    }

    // The valid list: every record type into buffer 0, then present it.
    if (Submit(BuildList(&column, &span, &sprite, &rect)) != MICROPIXEL_STATUS_OK) {
        return 66;
    }
    micropixel_surface_present_request_t present{};
    present.size = sizeof(present);
    present.surface = g_surface;
    present.buffer_index = 0U;
    present.pitch = g_width * 2U;
    present.src_width = g_width;
    present.src_height = g_height;
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &present, sizeof(present)) != MICROPIXEL_STATUS_OK) {
        return 67;
    }
    // In flight: the kernels may not touch it; the other buffer is free.
    if (Submit(BuildList(&column, &span)) != MICROPIXEL_STATUS_STALE_STATE) {
        return 68;
    }
    micropixel_raster_header_t header = Header(2U, BuildList(&column, &span));
    header.target_buffer = 1U;
    if (SubmitWithHeader(header, header.total_size) != MICROPIXEL_STATUS_OK) {
        return 69;
    }
    header.target_buffer = 2U;  // no such buffer
    if (SubmitWithHeader(header, header.total_size) != MICROPIXEL_STATUS_NOT_FOUND) {
        return 70;
    }

    // Draw list negatives.
    const uint32_t length = BuildList(&column, &span);
    header = Header(2U, length);
    header.target_buffer = 1U;
    header.magic ^= 1U;
    if (SubmitWithHeader(header, length) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 71;
    }
    header = Header(2U, length);
    header.target_buffer = 1U;
    header.total_size = length - 4U;
    if (SubmitWithHeader(header, length) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 72;
    }
    header = Header(2U, length);
    header.target_buffer = 1U;
    header.reserved0 = 1U;
    if (SubmitWithHeader(header, length) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 73;
    }
    header = Header(2U, length);
    header.target_buffer = 1U;
    header.target_pitch = static_cast<uint16_t>(g_width * 2U - 2U);  // narrower than the width
    if (SubmitWithHeader(header, length) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 74;
    }
    header = Header(2U, length);
    header.target_buffer = 1U;
    header.target_height = static_cast<uint16_t>(g_height / 2U);  // not the buffer's geometry
    if (SubmitWithHeader(header, length) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 75;
    }
    // Record negatives go to the free buffer 1.
    g_target_buffer = 1U;

    micropixel_raster_column_t bad_column = column;
    bad_column.y1 = static_cast<int16_t>(g_height);  // one past the last row
    if (Submit(BuildList(&bad_column, nullptr)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 76;
    }
    bad_column = column;
    bad_column.texture = 1U;  // row-major slot used as a column texture
    if (Submit(BuildList(&bad_column, nullptr)) != MICROPIXEL_STATUS_NOT_FOUND) {
        return 77;
    }
    bad_column = column;
    bad_column.light = kLightLevels;
    if (Submit(BuildList(&bad_column, nullptr)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 78;
    }
    micropixel_raster_span_pair_t bad_span = span;
    bad_span.x1 = static_cast<uint16_t>(g_width);
    if (Submit(BuildList(nullptr, &bad_span)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 79;
    }
    bad_span = span;
    bad_span.floor_texture = 7U;  // empty slot
    if (Submit(BuildList(nullptr, &bad_span)) != MICROPIXEL_STATUS_NOT_FOUND) {
        return 80;
    }
    micropixel_raster_sprite_t bad_sprite = sprite;
    bad_sprite.u0 = kTexSize;  // source rectangle past the texture
    if (Submit(BuildList(nullptr, nullptr, &bad_sprite)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 81;
    }
    bad_sprite = sprite;
    bad_sprite.texture = 1U;  // row-major slot in a sprite
    if (Submit(BuildList(nullptr, nullptr, &bad_sprite)) != MICROPIXEL_STATUS_NOT_FOUND) {
        return 85;
    }
    bad_sprite = sprite;
    bad_sprite.flags = 0x80U;  // unknown flag
    if (Submit(BuildList(nullptr, nullptr, &bad_sprite)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 86;
    }
    bad_sprite = sprite;
    bad_sprite.light = kLightLevels;
    if (Submit(BuildList(nullptr, nullptr, &bad_sprite)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 87;
    }
    // SOLID_COLOR ignores light: the same record is fine.
    bad_sprite.flags |= MICROPIXEL_RASTER_SPRITE_SOLID_COLOR;
    if (Submit(BuildList(nullptr, nullptr, &bad_sprite)) != MICROPIXEL_STATUS_OK) {
        return 88;
    }
    micropixel_raster_rect_t bad_rect = rect;
    bad_rect.alpha = 0U;
    if (Submit(BuildList(nullptr, nullptr, nullptr, &bad_rect)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 89;
    }
    bad_rect = rect;
    bad_rect.width = 0U;
    if (Submit(BuildList(nullptr, nullptr, nullptr, &bad_rect)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 90;
    }
    bad_rect = rect;
    bad_rect.x = -100;  // wholly off screen: valid, draws nothing
    bad_rect.width = 50U;
    if (Submit(BuildList(nullptr, nullptr, nullptr, &bad_rect)) != MICROPIXEL_STATUS_OK) {
        return 91;
    }
    // An unknown record type is rejected.
    micropixel_raster_rect_t unknown = rect;
    unknown.type = 9U;
    if (Submit(BuildList(nullptr, nullptr, nullptr, &unknown)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 92;
    }

    micropixel_handle_request_t destroy{static_cast<uint16_t>(sizeof(destroy)), 0U, g_surface};
    if (Call(MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY, &destroy, sizeof(destroy)) != MICROPIXEL_STATUS_OK) {
        return 93;
    }
    // Without a surface the kernels have no target again.
    if (Submit(BuildList(nullptr, nullptr, nullptr, &rect)) != MICROPIXEL_STATUS_NOT_FOUND) {
        return 94;
    }
    return 0;
}
