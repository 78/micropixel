#include "runtime/display_context.hpp"
#include "runtime/service_binding.hpp"
#include "sdk/graphics.hpp"

using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::GraphicsService;
using micropixel::runtime::LoadPhysicalGraphicsInfo;

namespace micropixel {

// ---- Graphics 1.6 raster kernels ----------------------------------------
namespace {

// One draw list is open at a time (single-threaded Guest), so the wire buffer
// is a runtime static rather than part of the move-only list object; Begin()
// refuses a second list while raster_wire_open.
alignas(8) uint8_t raster_wire[MICROPIXEL_GRAPHICS_MAX_RASTER_BYTES]{};
bool raster_wire_open = false;

}  // namespace

Result<SurfaceRaster> Renderer::CreateSurfaceRaster() const {
    const micropixel_graphics_info_t& raw = LoadPhysicalGraphicsInfo();
    if (raw.raster_pool_bytes == 0U || raw.raster_max_textures == 0U || raw.raster_max_light_levels == 0U ||
        (GraphicsService().info.capabilities & MICROPIXEL_GRAPHICS_CAP_RASTER) == 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    return SurfaceRaster{raw.raster_pool_bytes, raw.raster_max_textures, raw.raster_max_light_levels};
}

Result<void> SurfaceRaster::UploadTexture(uint8_t slot, uint32_t width, uint32_t height, RasterLayout layout,
                                          const uint8_t* texels) const {
    if (!valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    if (texels == nullptr || slot >= max_textures_ || width < MICROPIXEL_GRAPHICS_RASTER_MIN_TEXTURE_SIZE ||
        width > MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURE_SIZE || (width & (width - 1U)) != 0U ||
        height < MICROPIXEL_GRAPHICS_RASTER_MIN_TEXTURE_SIZE || height > MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURE_SIZE ||
        (height & (height - 1U)) != 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_raster_texture_upload_request_t request{};
    request.size = sizeof(request);
    request.slot = slot;
    request.width = static_cast<uint16_t>(width);
    request.height = static_cast<uint16_t>(height);
    request.layout = static_cast<uint16_t>(layout);
    request.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texels));
    request.length = width * height;
    const int32_t status =
        CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_RASTER_TEXTURE_UPLOAD, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

Result<void> SurfaceRaster::UploadLitPalette(uint32_t light_levels, const uint16_t* entries) const {
    if (!valid()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_UNSUPPORTED));
    }
    if (entries == nullptr || light_levels == 0U || light_levels > max_light_levels_) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    micropixel_raster_palette_upload_request_t request{};
    request.size = sizeof(request);
    request.light_levels = static_cast<uint16_t>(light_levels);
    request.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(entries));
    request.length = light_levels * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES * sizeof(uint16_t);
    const int32_t status =
        CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_RASTER_PALETTE_UPLOAD, &request, sizeof(request));
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

RasterDrawList SurfaceRaster::Begin(DirectSurface& surface, uint32_t buffer_index) const {
    if (!valid() || !surface.valid() || !surface.host_buffers() || buffer_index >= surface.buffer_count() ||
        surface.Busy(buffer_index) || raster_wire_open) {
        return RasterDrawList{};
    }
    return RasterDrawList{buffer_index, static_cast<uint16_t>(surface.buffer_width()),
                          static_cast<uint16_t>(surface.buffer_height()), static_cast<uint16_t>(surface.pitch())};
}

RasterDrawList::RasterDrawList(uint32_t target_buffer, uint16_t width, uint16_t height, uint16_t pitch)
    : target_buffer_(target_buffer),
      open_(true),
      width_(width),
      height_(height),
      pitch_(pitch),
      wire_size_(sizeof(micropixel_raster_header_t)) {
    raster_wire_open = true;
}

RasterDrawList::~RasterDrawList() { Close(); }

RasterDrawList::RasterDrawList(RasterDrawList&& other) noexcept
    : target_buffer_(other.target_buffer_),
      open_(other.open_),
      width_(other.width_),
      height_(other.height_),
      pitch_(other.pitch_),
      record_count_(other.record_count_),
      wire_size_(other.wire_size_),
      status_(other.status_) {
    other.open_ = false;  // ownership of the wire buffer moved here
    other.Close();
}

RasterDrawList& RasterDrawList::operator=(RasterDrawList&& other) noexcept {
    if (this != &other) {
        Close();  // drop whatever this list held (and its wire ownership)
        target_buffer_ = other.target_buffer_;
        open_ = other.open_;
        width_ = other.width_;
        height_ = other.height_;
        pitch_ = other.pitch_;
        record_count_ = other.record_count_;
        wire_size_ = other.wire_size_;
        status_ = other.status_;
        other.open_ = false;  // ownership of the wire buffer moved here
        other.Close();
    }
    return *this;
}

void RasterDrawList::Close() {
    if (open_) {
        raster_wire_open = false;
    }
    target_buffer_ = 0U;
    open_ = false;
    width_ = 0U;
    height_ = 0U;
    pitch_ = 0U;
    record_count_ = 0U;
    wire_size_ = 0U;
    status_ = MICROPIXEL_STATUS_OK;
}

bool RasterDrawList::Append(const void* record, uint32_t size) {
    if (!open()) {
        status_ = MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return false;
    }
    if (wire_size_ + size > sizeof(raster_wire) || record_count_ == 0xFFFFU) {
        Flush();
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
    header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    header.total_size = wire_size_;
    header.target_buffer = target_buffer_;
    header.target_width = width_;
    header.target_height = height_;
    header.target_pitch = pitch_;
    header.record_count = record_count_;
    CopyBytes(raster_wire, &header, sizeof(header));
    const int32_t status = micropixel_service_submit(GraphicsService().info.handle, MICROPIXEL_GRAPHICS_CHANNEL_RASTER,
                                                     raster_wire, wire_size_);
    if (status != MICROPIXEL_STATUS_OK && status_ == MICROPIXEL_STATUS_OK) {
        status_ = status;
    }
    wire_size_ = sizeof(header);
    record_count_ = 0U;
}

bool RasterDrawList::Column(uint16_t x, int16_t y0, int16_t y1, uint8_t texture, uint8_t light, uint16_t u,
                            int32_t v_start, int32_t v_step, bool transparent) {
    micropixel_raster_column_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_COLUMN;
    record.flags = transparent ? MICROPIXEL_RASTER_COLUMN_TRANSPARENT_INDEX0 : 0U;
    record.texture = texture;
    record.light = light;
    record.x = x;
    record.y0 = y0;
    record.y1 = y1;
    record.u = u;
    record.v_start = v_start;
    record.v_step = v_step;
    return Append(&record, sizeof(record));
}

bool RasterDrawList::SpanPair(uint16_t y_floor, uint16_t y_ceiling, uint16_t x0, uint16_t x1, uint8_t floor_texture,
                              uint8_t ceiling_texture, uint8_t light, int32_t s, int32_t t, int32_t ds, int32_t dt) {
    micropixel_raster_span_pair_t record{};
    record.type = MICROPIXEL_RASTER_RECORD_SPAN_PAIR;
    record.floor_texture = floor_texture;
    record.ceiling_texture = ceiling_texture;
    record.y_floor = y_floor;
    record.y_ceiling = y_ceiling;
    record.x0 = x0;
    record.x1 = x1;
    record.light = light;
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

bool RasterDrawList::Sprite(Rect destination, uint8_t texture, uint8_t light, uint16_t u0, uint16_t v0,
                            uint16_t src_width, uint16_t src_height, bool transparent) {
    micropixel_raster_sprite_t record{};
    if (!ClampRasterRect(destination, record.x, record.y, record.width, record.height)) {
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    record.flags = transparent ? MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 : 0U;
    record.texture = texture;
    record.light = light;
    record.u0 = u0;
    record.v0 = v0;
    record.src_width = src_width;
    record.src_height = src_height;
    return Append(&record, sizeof(record));
}

bool RasterDrawList::SolidSprite(Rect destination, uint8_t texture, Color color, uint16_t u0, uint16_t v0,
                                 uint16_t src_width, uint16_t src_height, bool transparent) {
    micropixel_raster_sprite_t record{};
    if (!ClampRasterRect(destination, record.x, record.y, record.width, record.height)) {
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_SPRITE;
    record.flags = static_cast<uint8_t>(MICROPIXEL_RASTER_SPRITE_SOLID_COLOR |
                                        (transparent ? MICROPIXEL_RASTER_SPRITE_TRANSPARENT_INDEX0 : 0U));
    record.texture = texture;
    record.u0 = u0;
    record.v0 = v0;
    record.src_width = src_width;
    record.src_height = src_height;
    record.color = color.rgb565();
    return Append(&record, sizeof(record));
}

bool RasterDrawList::FillRect(Rect area, Color color, uint8_t alpha) {
    micropixel_raster_rect_t record{};
    if (alpha == 0U || !ClampRasterRect(area, record.x, record.y, record.width, record.height)) {
        return false;
    }
    record.type = MICROPIXEL_RASTER_RECORD_RECT;
    record.alpha = alpha;
    record.color = color.rgb565();
    return Append(&record, sizeof(record));
}

Result<void> RasterDrawList::Finish() {
    if (!open()) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    Flush();
    const int32_t status = status_;
    Close();
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : unexpected(ErrorFromStatus(status));
}

}  // namespace micropixel
