#ifndef MICROPIXEL_RUNTIME_GRAPHICS_RASTER_KERNELS_HPP
#define MICROPIXEL_RUNTIME_GRAPHICS_RASTER_KERNELS_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"

// Graphics 1.6 raster kernels: validation and execution of a Guest draw list
// against Host-owned INDEX8 textures and a lit palette. Pure C++ with no ESP
// dependencies so the Host unit tests can compare the kernels against a
// reference implementation. Callers resolve Guest pointers and own the
// resource storage; nothing here allocates.

namespace micropixel::runtime::raster {

// One texture slot. `pixels == nullptr` marks an empty slot. Width and height
// may be arbitrary positive dimensions; log2 values cache the power-of-two
// fast path (UINT8_MAX means non-power-of-two) so the span kernel does not
// recompute them per pixel.
struct Texture final {
    const uint8_t* pixels{};
    uint16_t width{};
    uint16_t height{};
    uint8_t log2_width{};
    uint8_t log2_height{};
    uint8_t layout{};  // micropixel_raster_texture_layout_t
};

// One palette slot: light_levels rows of MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES
// RGB565 values; `entries == nullptr` marks an empty slot. `byte_swapped` is
// bookkeeping for the owner (which order the stored entries are in); the
// kernels copy entries verbatim and never read it.
struct Palette final {
    const uint16_t* entries{};
    uint16_t light_levels{};
    bool byte_swapped{};
    [[nodiscard]] const uint16_t* Row(uint32_t light_level) const {
        return entries + light_level * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES;
    }
};

// One warp map slot: width * height entries encoded as micropixel_raster_warp_entry
// documents; `entries == nullptr` marks an empty slot. `max_light` is the
// highest light level any entry carries, so a WARP record is validated against
// its palette once rather than per pixel.
struct WarpMap final {
    const uint32_t* entries{};
    // Two values per row: first and one-past-last entry that is not skipped
    // (WarpRowSpan), so the kernel never walks the empty corners around a
    // disc. nullptr means every row is walked in full.
    const uint16_t* row_spans{};
    uint16_t width{};
    uint16_t height{};
    uint8_t max_light{};
};

// Non-skipped extent of one map row as [first, last); first == last for an
// all-skip row.
// One pass over `row_count` rows of entries: writes each row's non-skip
// span (two uint16 per row, first/last as in WarpRowSpan) and the largest
// light level. False when an entry has reserved bits set; spans written so
// far are then unspecified.
[[nodiscard]] bool WarpScanRows(const uint32_t* rows, uint32_t width, uint32_t row_count, uint16_t* spans,
                                uint8_t& max_light);
void WarpRowSpan(const uint32_t* row, uint32_t width, uint16_t& first, uint16_t& last);

using TextureResolver = bool (*)(void*, uint32_t, device::BitmapView&);

struct Resources final {
    const Texture* textures{};
    uint32_t texture_count{};
    const Palette* palettes{};
    uint32_t palette_count{};
    const WarpMap* warps{};
    uint32_t warp_count{};
    TextureResolver resolve_texture{};
    void* texture_context{};
    [[nodiscard]] const Texture* TextureAt(uint8_t texture_slot) const {
        return texture_slot < texture_count && textures != nullptr ? textures + texture_slot : nullptr;
    }
    // Only occupied slots are returned.
    [[nodiscard]] const Palette* PaletteAt(uint8_t palette_slot) const {
        const Palette* palette =
            palette_slot < palette_count && palettes != nullptr ? palettes + palette_slot : nullptr;
        return palette != nullptr && palette->entries != nullptr && palette->light_levels != 0U ? palette : nullptr;
    }
    [[nodiscard]] const WarpMap* WarpAt(uint8_t warp_slot) const {
        const WarpMap* warp = warp_slot < warp_count && warps != nullptr ? warps + warp_slot : nullptr;
        return warp != nullptr && warp->entries != nullptr ? warp : nullptr;
    }
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
// record needs, palette slot present with the light level below its count).
// SPRITE, RECT, IMAGE and WARP may extend past the target; the kernels clip
// them. Returns MICROPIXEL_STATUS_OK and fills `header_out` when the list may
// be executed; otherwise a status describing the first problem. The target
// buffer is not touched here.
[[nodiscard]] int32_t ValidateDrawList(const uint8_t* bytes, uint32_t length, const Target& target,
                                       const Resources& resources, micropixel_raster_header_t& header_out);

// Optional per-record-kind accounting for ExecuteDrawList. Indexed by
// MICROPIXEL_RASTER_RECORD_* (index 0 unused). `pixels` counts the pixels a
// record asked for before clipping, so time / pixels is comparable across
// kinds. `now_us` is read around every record; nullptr disables timing but
// still counts records and pixels.
struct ExecuteProfile final {
    static constexpr uint32_t kKinds = MICROPIXEL_RASTER_RECORD_WARP + 1U;
    uint64_t (*now_us)(){};
    uint32_t records[kKinds]{};
    uint64_t pixels[kKinds]{};
    uint64_t time_us[kKinds]{};
};

// Executes a list that ValidateDrawList accepted. `target.pixels` must cover
// target.pitch * target.height bytes. `profile` may be nullptr.
void ExecuteDrawList(const uint8_t* bytes, const micropixel_raster_header_t& header, const Target& target,
                     const Resources& resources, ExecuteProfile* profile = nullptr);

// Individual kernels, exposed for tests and for the Host-side reference path.
void DrawColumn(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_column_t& column);
void DrawSpanPair(const Target& target, const Texture& floor_texture, const Texture& ceiling_texture,
                  const uint16_t* lit, const micropixel_raster_span_pair_t& span);
// `lit` may be nullptr for a SOLID_COLOR sprite.
void DrawSprite(const Target& target, const Texture& texture, const uint16_t* lit,
                const micropixel_raster_sprite_t& sprite);
void DrawRect(const Target& target, const micropixel_raster_rect_t& rect);
void DrawImage(const Target& target, const device::BitmapView& texture, const micropixel_raster_image_t& image);
// `texture` is ROW_MAJOR with power-of-two dimensions and `palette` has more
// levels than `warp.max_light` (ValidateDrawList checks both).
void DrawWarp(const Target& target, const WarpMap& warp, const Texture& texture, const Palette& palette,
              const micropixel_raster_warp_t& record);

// Highest light level carried by `count` warp entries, or UINT8_MAX when an
// entry sets a reserved bit. Skipped entries do not count.
[[nodiscard]] uint8_t WarpMaxLight(const uint32_t* entries, uint32_t count);

// UINT8_MAX for zero or non-power-of-two dimensions.
[[nodiscard]] uint8_t Log2Exact(uint32_t power_of_two);

}  // namespace micropixel::runtime::raster

#endif
