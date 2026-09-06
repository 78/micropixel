#include "runtime/services/raster_service.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

// Only referenced from the Kconfig-gated log paths below.
[[maybe_unused]] constexpr const char* kTag = "raster_svc";

}  // namespace

RasterService::RasterService(uint32_t pool_bytes) : pool_bytes_(pool_bytes) {}

RasterService::~RasterService() { Shutdown(); }

uint8_t* RasterService::Allocate(uint32_t bytes) {
    if (bytes == 0U || bytes > pool_bytes_ - used_bytes_) {
        return nullptr;
    }
    // The kernels touch one texel per output pixel, so PSRAM-resident tables
    // run within noise of internal SRAM once the L1 cache is warm. Internal
    // SRAM placement is opt-in per board because a Guest session leaves only
    // tens of KiB of it free.
    uint8_t* pixels = nullptr;
#if CONFIG_MICROPIXEL_RASTER_POOL_INTERNAL_SRAM
    pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
    if (pixels == nullptr) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#if CONFIG_MICROPIXEL_RASTER_POOL_INTERNAL_SRAM
        if (pixels != nullptr) {
            ESP_LOGW(kTag, "%" PRIu32 " B raster resource placed in PSRAM (internal SRAM exhausted)", bytes);
        }
#endif
    }
    if (pixels != nullptr) {
        used_bytes_ += bytes;
    }
    return pixels;
}

void RasterService::Release(Slot& slot) {
    if (slot.pixels != nullptr) {
        heap_caps_free(slot.pixels);
        used_bytes_ -= slot.bytes;
    }
    slot = {};
}

const uint8_t* RasterService::ResolveGuest(uint32_t offset, uint32_t length) const {
    uint8_t* source = nullptr;
    if (memory_.resolve == nullptr || !memory_.resolve(memory_.context, offset, length, &source)) {
        return nullptr;
    }
    return source;
}

uint8_t* RasterService::Replace(Slot& slot, uint32_t bytes) {
    // Allocate before releasing so a refused upload keeps the old resource;
    // only when the pool cannot hold both does the old one go first.
    uint8_t* pixels = Allocate(bytes);
    if (pixels == nullptr && slot.pixels != nullptr && bytes <= pool_bytes_ - (used_bytes_ - slot.bytes)) {
        Release(slot);
        pixels = Allocate(bytes);
    }
    if (pixels == nullptr) {
        return nullptr;
    }
    Release(slot);
    slot = {.pixels = pixels, .bytes = bytes};
    return pixels;
}

ServiceResult<void> RasterService::UploadTexture(const micropixel_raster_texture_upload_request_t& request) {
    if (!available()) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (request.size != sizeof(request) || request.reserved0 != 0U ||
        request.slot >= MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES || !raster::ValidTextureDimension(request.width) ||
        !raster::ValidTextureDimension(request.height) ||
        (request.layout != MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR &&
         request.layout != MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR) ||
        request.length != static_cast<uint32_t>(request.width) * request.height) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    // Validate the source before touching the slot: a refused upload leaves
    // the previous texture in place.
    const uint8_t* source = ResolveGuest(request.pixels, request.length);
    if (source == nullptr) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_MEMORY);
    }
    uint8_t* pixels = Replace(slots_[request.slot], request.length);
    if (pixels == nullptr) {
        // Replace may have had to free the old texture to make room and then
        // failed anyway; the slot must not keep pointing at freed memory.
        if (slots_[request.slot].pixels == nullptr) {
            textures_[request.slot] = {};
        }
        return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    std::memcpy(pixels, source, request.length);
    textures_[request.slot] = raster::Texture{
        .pixels = pixels,
        .width = request.width,
        .height = request.height,
        .log2_width = raster::Log2Exact(request.width),
        .log2_height = raster::Log2Exact(request.height),
        .layout = static_cast<uint8_t>(request.layout),
    };
    return {};
}

ServiceResult<void> RasterService::UploadPalette(const micropixel_raster_palette_upload_request_t& request) {
    if (!available()) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    const uint32_t bytes =
        static_cast<uint32_t>(request.light_levels) * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES * sizeof(uint16_t);
    if (request.size != sizeof(request) || request.light_levels == 0U ||
        request.light_levels > MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS || request.length != bytes) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint8_t* source = ResolveGuest(request.pixels, bytes);
    if (source == nullptr) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_MEMORY);
    }
    uint8_t* entries = Replace(palette_slot_, bytes);
    if (entries == nullptr) {
        // As for textures: the previous palette stays usable unless Replace
        // had to release it.
        if (palette_slot_.pixels == nullptr) {
            palette_levels_ = 0U;
        }
        return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    std::memcpy(entries, source, bytes);
    palette_levels_ = request.light_levels;
    palette_swapped_ = false;
    return {};
}

void RasterService::MatchPaletteByteOrder(bool byte_swapped) {
    if (palette_slot_.pixels == nullptr || palette_swapped_ == byte_swapped) {
        return;
    }
    // Entries arrive canonical; the kernels copy them into the target
    // verbatim, so store them in the target's order. Once per upload.
    auto* entries = reinterpret_cast<uint16_t*>(palette_slot_.pixels);
    const uint32_t count = static_cast<uint32_t>(palette_levels_) * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES;
    for (uint32_t index = 0U; index < count; ++index) {
        entries[index] = static_cast<uint16_t>((entries[index] << 8U) | (entries[index] >> 8U));
    }
    palette_swapped_ = byte_swapped;
}

raster::Resources RasterService::ResourcesView() const {
    return raster::Resources{
        .textures = textures_,
        .texture_count = MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES,
        .palette = {.entries = reinterpret_cast<const uint16_t*>(palette_slot_.pixels),
                    .light_levels = palette_levels_},
    };
}

ServiceResult<void> RasterService::Submit(const uint8_t* bytes, uint32_t length, const DirectSurfaceService& surfaces) {
    if (!available()) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    const raster::Resources resources = ResourcesView();
    micropixel_raster_header_t header{};
    const int32_t status = raster::ValidateDrawList(bytes, length, resources, header);
    if (status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(status);
    }
    HostBufferView view{};
    const int32_t buffer_status = surfaces.HostBuffer(header.target_buffer, view);
    if (buffer_status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(buffer_status);
    }
    // The records were validated against the header's geometry; it has to be
    // the buffer's.
    if (view.width != header.target_width || view.height != header.target_height || view.pitch != header.target_pitch) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    MatchPaletteByteOrder(surfaces.native_byte_swapped());
    const raster::Target target{
        .pixels = view.pixels,
        .width = view.width,
        .height = view.height,
        .pitch = view.pitch,
        .byte_swapped = surfaces.native_byte_swapped(),
    };
#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
    // One short line every kTelemetrySubmits submissions: kernel time per
    // submit is the number to compare against the Guest's render_avg.
    const int64_t started_us = esp_timer_get_time();
    raster::ExecuteDrawList(bytes, header, target, resources);
    telemetry_execute_us_ += static_cast<uint64_t>(esp_timer_get_time() - started_us);
    telemetry_records_ += header.record_count;
    telemetry_bytes_ += length;
    if (++telemetry_submits_ == kTelemetrySubmits) {
        ESP_LOGI(kTag,
                 "raster: submits=%" PRIu32 " avg_us=%" PRIu32 " records/submit=%" PRIu32 " bytes/submit=%" PRIu32,
                 telemetry_submits_, static_cast<uint32_t>(telemetry_execute_us_ / telemetry_submits_),
                 telemetry_records_ / telemetry_submits_, telemetry_bytes_ / telemetry_submits_);
        telemetry_submits_ = 0U;
        telemetry_records_ = 0U;
        telemetry_bytes_ = 0U;
        telemetry_execute_us_ = 0U;
    }
#else
    raster::ExecuteDrawList(bytes, header, target, resources);
#endif
    return {};
}

void RasterService::Shutdown() {
    for (uint32_t index = 0U; index < MICROPIXEL_GRAPHICS_RASTER_MAX_TEXTURES; ++index) {
        Release(slots_[index]);
        textures_[index] = {};
    }
    Release(palette_slot_);
    palette_levels_ = 0U;
    palette_swapped_ = false;
}

}  // namespace micropixel::runtime
