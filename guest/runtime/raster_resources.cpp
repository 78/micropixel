#include "runtime/display_context.hpp"
#include "runtime/display_transform.hpp"
#include "runtime/service_binding.hpp"
#include "runtime/texture_state.hpp"
#include "sdk/graphics.hpp"
#include "sdk/resources.hpp"

using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::GraphicsService;
using micropixel::runtime::LoadGraphicsLimits;
using micropixel::runtime::OpenService;

namespace micropixel {

// ---- Raster kernels -----------------------------------------------------
namespace {

static_assert(RasterResources::kPaletteEntries == MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES,
              "SDK palette entry count must match the ABI");

// One draw list is open at a time (single-threaded Guest), so the wire buffer
// is a runtime static rather than part of the callback-scoped list object; BeginUpdate()
// refuses a second list while raster_wire_open.
alignas(8) uint8_t raster_wire[micropixel::runtime::limits::kMaxRasterBytes]{};
bool raster_wire_open = false;

}  // namespace

Result<RasterResources> Renderer::CreateRasterResources() const {
    const int32_t status = OpenService(GraphicsService(), MICROPIXEL_SERVICE_GRAPHICS,
                                       MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if ((GraphicsService().info.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER) == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    return RasterResources{true};
}

Result<void> RasterResources::UploadTexture(uint8_t texture_slot, uint32_t width, uint32_t height, RasterLayout layout,
                                            std::span<const uint8_t> texels) const {
    if (!valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    if (texels.data() == nullptr || width == 0U || height == 0U || width > UINT16_MAX || height > UINT16_MAX ||
        texels.size() != static_cast<size_t>(width) * height ||
        (layout != RasterLayout::kColumnMajor && layout != RasterLayout::kRowMajor)) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_raster_texture_upload_request_t request{};
    request.size = sizeof(request);
    request.texture_slot = texture_slot;
    request.width = static_cast<uint16_t>(width);
    request.height = static_cast<uint16_t>(height);
    request.layout = static_cast<uint16_t>(layout);
    request.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texels.data()));
    request.length = width * height;
    const int32_t status =
        CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

Result<void> RasterResources::UploadLitPalette(uint8_t palette_slot, uint32_t light_levels,
                                               std::span<const uint16_t> entries) const {
    if (!valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    if (entries.data() == nullptr || light_levels == 0U || light_levels > MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS ||
        entries.size() != static_cast<size_t>(light_levels) * kPaletteEntries) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_raster_palette_upload_request_t request{};
    request.size = sizeof(request);
    request.palette_slot = palette_slot;
    request.light_levels = static_cast<uint16_t>(light_levels);
    request.entries = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entries.data()));
    request.length = static_cast<uint32_t>(entries.size_bytes());
    const int32_t status =
        CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

Result<void> RasterResources::UploadWarpMap(uint8_t warp_slot, uint32_t width, uint32_t height,
                                            std::span<const uint32_t> entries) const {
    return UpdateWarpRows(warp_slot, width, height, 0U, height, entries);
}

Result<void> RasterResources::UpdateWarpRows(uint8_t warp_slot, uint32_t width, uint32_t height, uint32_t first_row,
                                             uint32_t row_count, std::span<const uint32_t> entries) const {
    if (!valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    if (entries.data() == nullptr || width == 0U || height == 0U || width > UINT16_MAX || height > UINT16_MAX ||
        row_count == 0U || first_row >= height || row_count > height - first_row ||
        entries.size() != static_cast<size_t>(width) * row_count) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_raster_warp_upload_request_t request{};
    request.size = sizeof(request);
    request.warp_slot = warp_slot;
    request.width = static_cast<uint16_t>(width);
    request.height = static_cast<uint16_t>(height);
    request.row0 = static_cast<uint16_t>(first_row);
    request.row_count = static_cast<uint16_t>(row_count);
    request.entries = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entries.data()));
    request.length = static_cast<uint32_t>(entries.size_bytes());
    const int32_t status =
        CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_RASTER_WARP_UPLOAD, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

RasterDrawList HostSurface::BeginUpdate(uint32_t buffer_index) const {
    if (!valid()) return RasterDrawList{MICROPIXEL_STATUS_STALE_STATE};
    if ((GraphicsService().info.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER) == 0U) {
        return RasterDrawList{MICROPIXEL_STATUS_UNSUPPORTED};
    }
    if (buffer_index >= buffer_count()) return RasterDrawList{MICROPIXEL_STATUS_INVALID_ARGUMENT};
    if (Busy(buffer_index) || raster_wire_open) return RasterDrawList{MICROPIXEL_STATUS_STALE_STATE};
    return RasterDrawList{handle_, buffer_index};
}

RasterDrawList::RasterDrawList(uint32_t surface_handle, uint32_t buffer_index)
    : surface_handle_(surface_handle),
      target_buffer_(buffer_index),
      open_(true),
      wire_size_(sizeof(micropixel_raster_header_t)) {
    raster_wire_open = true;
}

RasterDrawList::~RasterDrawList() { Close(); }

void RasterDrawList::Close() {
    if (open_) {
        raster_wire_open = false;
    }
    surface_handle_ = 0U;
    target_buffer_ = 0U;
    open_ = false;
    palette_slot_ = 0U;
    record_count_ = 0U;
    wire_size_ = 0U;
    status_ = MICROPIXEL_STATUS_OK;
}

bool RasterDrawList::Append(const void* record, uint32_t size) {
    if (status_ != MICROPIXEL_STATUS_OK) return false;
    if (!open()) {
        status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    if (wire_size_ + size > LoadGraphicsLimits().max_raster_bytes || record_count_ == 0xFFFFU) {
        Flush();
        if (status_ != MICROPIXEL_STATUS_OK) return false;
    }
    CopyBytes(raster_wire + wire_size_, record, size);
    wire_size_ += size;
    ++record_count_;
    return status_ == MICROPIXEL_STATUS_OK;
}

void RasterDrawList::Flush() {
    if (record_count_ == 0U) {
        return;
    }
    micropixel_raster_header_t header{};
    header.magic = MICROPIXEL_GRAPHICS_RASTER_MAGIC;
    header.total_size = wire_size_;
    header.surface_handle = surface_handle_;
    header.buffer_index = target_buffer_;
    header.record_count = record_count_;
    CopyBytes(raster_wire, &header, sizeof(header));
    const int32_t status = micropixel_service_submit(GraphicsService().info.service_handle,
                                                     MICROPIXEL_GRAPHICS_CHANNEL_RASTER, raster_wire, wire_size_);
    if (status != MICROPIXEL_STATUS_OK && status_ == MICROPIXEL_STATUS_OK) {
        status_ = status;
    }
    wire_size_ = sizeof(header);
    record_count_ = 0U;
}

bool RasterDrawList::Column(uint16_t x, int16_t y0, int16_t y1, uint8_t texture_slot, uint8_t light_level, uint16_t u,
                            int32_t v_start, int32_t v_step, bool transparent) {
    micropixel_raster_column_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_COLUMN;
    record.flags = transparent ? MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0 : 0U;
    record.texture_slot = texture_slot;
    record.light_level = light_level;
    record.x = x;
    record.y0 = y0;
    record.y1 = y1;
    record.u = u;
    record.v_start = v_start;
    record.v_step = v_step;
    record.palette_slot = palette_slot_;
    return Append(&record, sizeof(record));
}

bool RasterDrawList::SpanPair(uint16_t y_floor, uint16_t y_ceiling, uint16_t x0, uint16_t x1,
                              uint8_t floor_texture_slot, uint8_t ceiling_texture_slot, uint8_t light_level, int32_t s,
                              int32_t t, int32_t ds, int32_t dt) {
    micropixel_raster_span_pair_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_SPAN_PAIR;
    record.floor_texture_slot = floor_texture_slot;
    record.ceiling_texture_slot = ceiling_texture_slot;
    record.y_floor = y_floor;
    record.y_ceiling = y_ceiling;
    record.x0 = x0;
    record.x1 = x1;
    record.light_level = light_level;
    record.palette_slot = palette_slot_;
    record.s = s;
    record.t = t;
    record.ds = ds;
    record.dt = dt;
    return Append(&record, sizeof(record));
}

namespace {

// Clamps a Rect to the int16/uint16 fields of a raster record. Anything wider
// than the field range cannot be meant literally; the Host clips the rest.
bool ClampRasterRect(Rect rect, int16_t& x, int16_t& y, uint16_t& width, uint16_t& height) {
    if (rect.width <= 0 || rect.height <= 0 || rect.x < INT16_MIN || rect.x > INT16_MAX || rect.y < INT16_MIN ||
        rect.y > INT16_MAX) {
        return false;
    }
    x = static_cast<int16_t>(rect.x);
    y = static_cast<int16_t>(rect.y);
    width = static_cast<uint16_t>(rect.width > UINT16_MAX ? UINT16_MAX : rect.width);
    height = static_cast<uint16_t>(rect.height > UINT16_MAX ? UINT16_MAX : rect.height);
    return true;
}

}  // namespace

bool RasterDrawList::Sprite(Rect destination, uint8_t texture_slot, uint8_t light_level, uint16_t u0, uint16_t v0,
                            uint16_t source_width, uint16_t source_height, bool transparent) {
    micropixel_raster_sprite_t record{};
    if (!ClampRasterRect(destination, record.x, record.y, record.width, record.height)) {
        if (status_ == MICROPIXEL_STATUS_OK) status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    record.flags = transparent ? MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 : 0U;
    record.texture_slot = texture_slot;
    record.light_level = light_level;
    record.palette_slot = palette_slot_;
    record.source_x = u0;
    record.source_y = v0;
    record.source_width = source_width;
    record.source_height = source_height;
    return Append(&record, sizeof(record));
}

namespace {

// The public header spells the entry layout without the ABI header.
static_assert(WarpEntry::kSkip == MICROPIXEL_RASTER_WARP_ENTRY_SKIP);
static_assert(WarpEntry::kSolid == MICROPIXEL_RASTER_WARP_ENTRY_SOLID);
static_assert(WarpEntry::kMaxLight == MICROPIXEL_RASTER_WARP_LIGHT_MASK);
static_assert(WarpEntry::kMaxCoordinate == MICROPIXEL_RASTER_WARP_U_MASK &&
              WarpEntry::kMaxCoordinate == MICROPIXEL_RASTER_WARP_V_MASK);
static_assert(WarpEntry::Texel(5U, 7U, 3U) ==
              ((3U << MICROPIXEL_RASTER_WARP_LIGHT_SHIFT) | (7U << MICROPIXEL_RASTER_WARP_V_SHIFT) | 5U));

bool EncodeWarp(micropixel_raster_warp_t& record, Point origin, uint8_t warp_slot, uint8_t texture_slot,
                uint8_t palette_slot, uint16_t u_offset, uint16_t v_offset, uint8_t u_fraction_bits) {
    if (origin.x < INT16_MIN || origin.x > INT16_MAX || origin.y < INT16_MIN || origin.y > INT16_MAX ||
        u_fraction_bits > MICROPIXEL_RASTER_WARP_MAX_U_FRACTION_BITS) {
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_WARP;
    record.warp_slot = warp_slot;
    record.texture_slot = texture_slot;
    record.palette_slot = palette_slot;
    record.x = static_cast<int16_t>(origin.x);
    record.y = static_cast<int16_t>(origin.y);
    record.u_offset = u_offset;
    record.v_offset = v_offset;
    record.u_fraction_bits = u_fraction_bits;
    return true;
}

}  // namespace

bool RasterDrawList::Warp(Point origin, uint8_t warp_slot, uint8_t texture_slot, uint16_t u_offset, uint16_t v_offset,
                          uint8_t u_fraction_bits) {
    micropixel_raster_warp_t record{};
    if (!EncodeWarp(record, origin, warp_slot, texture_slot, palette_slot_, u_offset, v_offset, u_fraction_bits)) {
        if (status_ == MICROPIXEL_STATUS_OK) status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    return Append(&record, sizeof(record));
}

bool RasterDrawList::Warp(Point origin, uint8_t warp_slot, uint8_t texture_slot, Color fill, uint16_t u_offset,
                          uint16_t v_offset, uint8_t u_fraction_bits) {
    micropixel_raster_warp_t record{};
    if (!EncodeWarp(record, origin, warp_slot, texture_slot, palette_slot_, u_offset, v_offset, u_fraction_bits)) {
        if (status_ == MICROPIXEL_STATUS_OK) status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    record.flags = MICROPIXEL_RASTER_WARP_FILL_SKIPPED;
    record.fill_color = fill.rgb565();
    return Append(&record, sizeof(record));
}

bool RasterDrawList::Image(const Texture& texture, Rect destination, Rect source, uint8_t opacity) {
    micropixel_raster_image_t record{};
    if (!texture.valid() || destination.width > UINT16_MAX || destination.height > UINT16_MAX ||
        !ClampRasterRect(destination, record.x, record.y, record.width, record.height) || source.x < 0 ||
        source.y < 0 || source.width <= 0 || source.height <= 0 ||
        static_cast<uint64_t>(source.x) + source.width > texture.width_ ||
        static_cast<uint64_t>(source.y) + source.height > texture.height_) {
        if (status_ == MICROPIXEL_STATUS_OK) status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    const auto physical = detail::MapTextureRect(source.x, source.y, source.width, source.height, texture.width_,
                                                 texture.height_, texture.physical_width_, texture.physical_height_);
    record.type = MICROPIXEL_RASTER_RECORD_IMAGE;
    record.texture_handle = runtime::TextureSnapshot(texture.handle_);
    record.opacity = opacity;
    record.source_x = physical.x;
    record.source_y = physical.y;
    record.source_width = physical.width;
    record.source_height = physical.height;
    return Append(&record, sizeof(record));
}

bool RasterDrawList::SolidSprite(Rect destination, uint8_t texture_slot, Color color, uint16_t u0, uint16_t v0,
                                 uint16_t source_width, uint16_t source_height, bool transparent) {
    micropixel_raster_sprite_t record{};
    if (!ClampRasterRect(destination, record.x, record.y, record.width, record.height)) {
        if (status_ == MICROPIXEL_STATUS_OK) status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    record.flags = static_cast<uint8_t>(MICROPIXEL_RASTER_SPRITE_SOLID_COLOR |
                                        (transparent ? MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 : 0U));
    record.texture_slot = texture_slot;
    record.source_x = u0;
    record.source_y = v0;
    record.source_width = source_width;
    record.source_height = source_height;
    record.color = color.rgb565();
    return Append(&record, sizeof(record));
}

bool RasterDrawList::FillRect(Rect area, Color color, uint8_t alpha) {
    micropixel_raster_rect_t record{};
    if (alpha == 0U || !ClampRasterRect(area, record.x, record.y, record.width, record.height)) {
        if (status_ == MICROPIXEL_STATUS_OK) status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_RECT;
    record.opacity = alpha;
    record.color = color.rgb565();
    return Append(&record, sizeof(record));
}

namespace {

// Wire vertices are a plain copy of the public ones; keep the layouts in step.
static_assert(sizeof(RasterVertex) == sizeof(micropixel_raster_vertex_t));
static_assert(RasterVertex::kPositionScale == 16 && RasterVertex::kTexelScale == 256);

void EncodeVertices(const RasterVertex* corners, uint32_t count, micropixel_raster_vertex_t* wire) {
    for (uint32_t index = 0U; index < count; ++index) {
        wire[index].x = corners[index].x;
        wire[index].y = corners[index].y;
        wire[index].u = corners[index].u;
        wire[index].v = corners[index].v;
        wire[index].light = corners[index].light;
        wire[index].reserved0 = 0U;
    }
}

}  // namespace

bool RasterDrawList::Triangle(const RasterVertex (&corners)[3], uint8_t texture_slot, bool transparent) {
    micropixel_raster_triangle_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_TRIANGLE;
    record.flags = transparent ? MICROPIXEL_RASTER_POLYGON_TRANSPARENT_INDEX0 : 0U;
    record.texture_slot = texture_slot;
    record.palette_slot = palette_slot_;
    EncodeVertices(corners, 3U, record.vertices);
    return Append(&record, sizeof(record));
}

bool RasterDrawList::Quad(const RasterVertex (&corners)[4], uint8_t texture_slot, bool transparent) {
    micropixel_raster_quad_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_QUAD;
    record.flags = transparent ? MICROPIXEL_RASTER_POLYGON_TRANSPARENT_INDEX0 : 0U;
    record.texture_slot = texture_slot;
    record.palette_slot = palette_slot_;
    EncodeVertices(corners, 4U, record.vertices);
    return Append(&record, sizeof(record));
}

bool RasterDrawList::FlatTriangle(const RasterVertex (&corners)[3], uint8_t color_index) {
    micropixel_raster_triangle_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_TRIANGLE;
    record.flags = MICROPIXEL_RASTER_POLYGON_FLAT_COLOR;
    record.palette_slot = palette_slot_;
    EncodeVertices(corners, 3U, record.vertices);
    record.vertices[0].u = static_cast<uint16_t>(color_index << 8U);
    return Append(&record, sizeof(record));
}

bool RasterDrawList::FlatQuad(const RasterVertex (&corners)[4], uint8_t color_index) {
    micropixel_raster_quad_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_QUAD;
    record.flags = MICROPIXEL_RASTER_POLYGON_FLAT_COLOR;
    record.palette_slot = palette_slot_;
    EncodeVertices(corners, 4U, record.vertices);
    record.vertices[0].u = static_cast<uint16_t>(color_index << 8U);
    return Append(&record, sizeof(record));
}

Result<void> RasterDrawList::Finish() {
    if (!open()) {
        return unexpected(ErrorFromStatus(status_));
    }
    if (status_ == MICROPIXEL_STATUS_OK) Flush();
    const int32_t status = status_;
    Close();
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

}  // namespace micropixel
