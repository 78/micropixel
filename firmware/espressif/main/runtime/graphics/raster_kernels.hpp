#ifndef MICROPIXEL_RUNTIME_GRAPHICS_RASTER_KERNELS_HPP
#define MICROPIXEL_RUNTIME_GRAPHICS_RASTER_KERNELS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

// Graphics 1.6 raster kernels: validation and execution of a Guest draw list
// against Host-owned INDEX8 textures and a lit palette. Pure C++ with no ESP
// dependencies so the Host unit tests can compare the kernels against a
// reference implementation. Callers resolve Guest pointers and own the
// resource storage; nothing here allocates.

namespace micropixel::runtime::raster {

// One texture slot. `pixels == nullptr` marks an empty slot. Width and height
// are powers of two; log2 values are cached so the span kernel does not
// recompute them per pixel.
struct Texture final {
    const uint8_t* pixels{};
    uint16_t width{};
    uint16_t height{};
    uint8_t log2_width{};
    uint8_t log2_height{};
    uint8_t layout{};  // micropixel_raster_texture_layout_t
};

// light_levels rows of MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES RGB565
// values; `entries == nullptr` when no palette is uploaded.
struct Palette final {
    const uint16_t* entries{};
    uint16_t light_levels{};
};

struct Resources final {
    const Texture* textures{};
    uint32_t texture_count{};
    Palette palette{};
};

// RGB565 destination named by the draw-list header and resolved by the caller.
// `byte_swapped`: the buffer (and the lit palette) hold panel order with the
// two bytes of every pixel swapped; record colors are canonical and converted.
struct Target final {
    uint8_t* pixels{};
    uint32_t width{};
    uint32_t height{};
    uint32_t pitch{};
    bool byte_swapped{};
};

// Parses and validates a complete draw list: header (magic, version, sizes,
// target geometry) and every record (type, sizes, coordinates inside the
// target for COLUMN/SPAN_PAIR, texture slots present with the layout the
// record needs, light below the palette level count). SPRITE and RECT may
// extend past the target; the kernels clip them. Returns MICROPIXEL_STATUS_OK
// and fills `header_out` when the list may be executed; otherwise a status
// describing the first problem. The target buffer is not touched here.
[[nodiscard]] int32_t ValidateDrawList(const uint8_t* bytes, uint32_t length, const Resources& resources,
                                       micropixel_raster_header_t& header_out);

// Executes a list that ValidateDrawList accepted. `target.pixels` must cover
// header.target_pitch * header.target_height bytes.
void ExecuteDrawList(const uint8_t* bytes, const micropixel_raster_header_t& header, const Target& target,
                     const Resources& resources);

// Individual kernels, exposed for tests and for the Host-side reference path.
void DrawColumn(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_column_t& column);
void DrawSpanPair(const Target& target, const Texture& floor_texture, const Texture& ceiling_texture,
                  const uint16_t* lit, const micropixel_raster_span_pair_t& span);
// `lit` may be nullptr for a SOLID_COLOR sprite.
void DrawSprite(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_sprite_t& sprite);
void DrawRect(const Target& target, const micropixel_raster_rect_t& rect);

// True when `value` is a power of two inside the ABI texture size range.
[[nodiscard]] bool ValidTextureDimension(uint32_t value);
[[nodiscard]] uint8_t Log2Exact(uint32_t power_of_two);

}  // namespace micropixel::runtime::raster

#endif
