#include "runtime/services/raster_service.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

// Only referenced from the Kconfig-gated log paths below.
[[maybe_unused]] constexpr const char* kTag = "raster_svc";

constexpr uint32_t kPaletteRowBytes = MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES * sizeof(uint16_t);

template <typename Entry>
void FreeSlots(Entry* entries, uint32_t capacity, auto pixels_of) {
    for (uint32_t index = 0U; index < capacity; ++index) {
        heap_caps_free(const_cast<void*>(static_cast<const void*>(pixels_of(entries[index]))));
    }
    heap_caps_free(entries);
}

}  // namespace

RasterService::RasterService(bool enabled) : enabled_(enabled) {}

RasterService::~RasterService() { Shutdown(); }

uint8_t* RasterService::Allocate(uint32_t bytes) {
    if (bytes == 0U) {
        return nullptr;
    }
    uint8_t* pixels = nullptr;
#if CONFIG_MICROPIXEL_RASTER_POOL_INTERNAL_SRAM
    const bool try_internal = true;
#else
    const bool try_internal = false;
#endif
    if (try_internal) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (pixels == nullptr) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (try_internal && pixels != nullptr) {
            ESP_LOGW(kTag, "%" PRIu32 " B raster resource placed in PSRAM (internal SRAM exhausted)", bytes);
        }
    }
    return pixels;
}

const uint8_t* RasterService::ResolveGuest(uint32_t offset, uint32_t length) const {
    uint8_t* source = nullptr;
    if (memory_.resolve == nullptr || !memory_.resolve(memory_.context, offset, length, &source)) {
        return nullptr;
    }
    return source;
}

template <typename Entry>
bool RasterService::Reserve(SlotTable<Entry>& table, uint32_t index) {
    if (index < table.capacity) return true;
    uint32_t capacity = std::max<uint32_t>(8U, table.capacity);
    while (index >= capacity) capacity *= 2U;
    const size_t bytes = static_cast<size_t>(capacity) * sizeof(Entry);
    auto* replacement = static_cast<Entry*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (replacement == nullptr) return false;
    for (uint32_t i = 0U; i < capacity; ++i) std::construct_at(replacement + i);
    if (table.capacity != 0U) std::copy_n(table.entries, table.capacity, replacement);
    heap_caps_free(table.entries);
    table.entries = replacement;
    table.capacity = capacity;
    return true;
}

ServiceResult<void> RasterService::UploadTexture(const micropixel_raster_texture_upload_request_t& request) {
    if (!available()) return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    if (request.size != sizeof(request) || request.reserved0 != 0U || request.reserved1 != 0U || request.width == 0U ||
        request.height == 0U || request.length != static_cast<uint32_t>(request.width) * request.height ||
        (request.layout != MICROPIXEL_RASTER_LAYOUT_COLUMN_MAJOR &&
         request.layout != MICROPIXEL_RASTER_LAYOUT_ROW_MAJOR)) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint8_t* source = ResolveGuest(request.pixels, request.length);
    if (source == nullptr) return FailService<void>(MICROPIXEL_STATUS_INVALID_MEMORY);
    // Pixels first, table second: whichever fails, nothing is left behind.
    uint8_t* pixels = Allocate(request.length);
    if (pixels == nullptr) return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    if (!Reserve(textures_, request.texture_slot)) {
        heap_caps_free(pixels);
        return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    std::memcpy(pixels, source, request.length);
    raster::Texture& slot = textures_.entries[request.texture_slot];
    heap_caps_free(const_cast<uint8_t*>(slot.pixels));
    slot = {.pixels = pixels,
            .width = request.width,
            .height = request.height,
            .log2_width = raster::Log2Exact(request.width),
            .log2_height = raster::Log2Exact(request.height),
            .layout = static_cast<uint8_t>(request.layout)};
    return {};
}

ServiceResult<void> RasterService::UploadPalette(const micropixel_raster_palette_upload_request_t& request) {
    if (!available()) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    const uint32_t bytes = static_cast<uint32_t>(request.light_levels) * kPaletteRowBytes;
    if (request.size != sizeof(request) || request.reserved0 != 0U || request.reserved1 != 0U ||
        request.light_levels == 0U || request.light_levels > MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS ||
        request.length != bytes) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint8_t* source = ResolveGuest(request.entries, bytes);
    if (source == nullptr) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_MEMORY);
    }
    uint8_t* entries = Allocate(bytes);
    if (entries == nullptr) return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    if (!Reserve(palettes_, request.palette_slot)) {
        heap_caps_free(entries);
        return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    std::memcpy(entries, source, bytes);
    raster::Palette& slot = palettes_.entries[request.palette_slot];
    heap_caps_free(const_cast<uint16_t*>(slot.entries));
    slot = {.entries = reinterpret_cast<const uint16_t*>(entries), .light_levels = request.light_levels};
    return {};
}

ServiceResult<void> RasterService::UploadWarp(const micropixel_raster_warp_upload_request_t& request) {
    if (!available()) return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    const uint32_t row_bytes = static_cast<uint32_t>(request.width) * sizeof(uint32_t);
    if (request.size != sizeof(request) || request.reserved0 != 0U || request.width == 0U || request.height == 0U ||
        request.row_count == 0U || request.row0 >= request.height ||
        request.row_count > static_cast<uint32_t>(request.height) - request.row0 ||
        request.length != row_bytes * request.row_count) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint8_t* source = ResolveGuest(request.entries, request.length);
    if (source == nullptr) return FailService<void>(MICROPIXEL_STATUS_INVALID_MEMORY);
    const auto* source_rows = reinterpret_cast<const uint32_t*>(source);
    uint8_t rows_light = 0U;
    raster::WarpMap* slot = request.warp_slot < warps_.capacity ? warps_.entries + request.warp_slot : nullptr;
    if (slot != nullptr && slot->entries != nullptr && slot->width == request.width && slot->height == request.height) {
        // Rows are replaced under the App's own sequencing; a partial update
        // can only raise the light ceiling, a full one recomputes it. The
        // source is scanned once for validity, spans and light, then copied;
        // a rejected scan restores the spans it overwrote from the old rows.
        auto* spans = const_cast<uint16_t*>(slot->row_spans) + static_cast<size_t>(request.row0) * 2U;
        if (!raster::WarpScanRows(source_rows, request.width, request.row_count, spans, rows_light) ||
            rows_light >= MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS) {
            RefreshWarpSpans(*slot, request.row0, request.row_count);
            return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        auto* entries = const_cast<uint32_t*>(slot->entries);
        std::memcpy(entries + static_cast<size_t>(request.row0) * request.width, source, request.length);
        slot->max_light = request.row_count == request.height ? rows_light : std::max(slot->max_light, rows_light);
        return {};
    }
    // A new map: rows the request does not cover start out skipped, so the
    // App can stream a large map in over several requests. The row spans live
    // in the same allocation, after the entries.
    const uint32_t map_bytes = row_bytes * request.height;
    const uint32_t span_bytes = static_cast<uint32_t>(request.height) * 2U * sizeof(uint16_t);
    uint8_t* entries = Allocate(map_bytes + span_bytes);
    if (entries == nullptr) return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    if (!Reserve(warps_, request.warp_slot)) {
        heap_caps_free(entries);
        return FailService<void>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    auto* map = reinterpret_cast<uint32_t*>(entries);
    auto* spans = reinterpret_cast<uint16_t*>(entries + map_bytes);
    if (!raster::WarpScanRows(source_rows, request.width, request.row_count,
                              spans + static_cast<size_t>(request.row0) * 2U, rows_light) ||
        rows_light >= MICROPIXEL_GRAPHICS_RASTER_MAX_LIGHT_LEVELS) {
        heap_caps_free(entries);
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    std::fill_n(map, static_cast<size_t>(request.row0) * request.width, MICROPIXEL_RASTER_WARP_ENTRY_SKIP);
    std::memcpy(map + static_cast<size_t>(request.row0) * request.width, source, request.length);
    const size_t covered = static_cast<size_t>(request.row0 + request.row_count) * request.width;
    std::fill_n(map + covered, static_cast<size_t>(request.height) * request.width - covered,
                MICROPIXEL_RASTER_WARP_ENTRY_SKIP);
    // Rows outside the request are all skip: empty spans.
    for (uint32_t row = 0U; row < request.height; ++row) {
        if (row >= request.row0 && row < request.row0 + request.row_count) continue;
        spans[row * 2U] = request.width;
        spans[row * 2U + 1U] = request.width;
    }
    slot = warps_.entries + request.warp_slot;
    heap_caps_free(const_cast<uint32_t*>(slot->entries));
    *slot = {
        .entries = map, .row_spans = spans, .width = request.width, .height = request.height, .max_light = rows_light};
    return {};
}

void RasterService::RefreshWarpSpans(raster::WarpMap& map, uint32_t row0, uint32_t row_count) {
    auto* spans = const_cast<uint16_t*>(map.row_spans);
    for (uint32_t row = row0; row < row0 + row_count; ++row) {
        raster::WarpRowSpan(map.entries + static_cast<size_t>(row) * map.width, map.width, spans[row * 2U],
                            spans[row * 2U + 1U]);
    }
}

void RasterService::MatchPaletteByteOrder(bool byte_swapped) {
    // Entries arrive canonical; the kernels copy them into the target
    // verbatim, so store them in the target's order. Once per upload.
    for (uint32_t index = 0U; index < palettes_.capacity; ++index) {
        raster::Palette& slot = palettes_.entries[index];
        if (slot.entries == nullptr || slot.byte_swapped == byte_swapped) continue;
        auto* entries = const_cast<uint16_t*>(slot.entries);
        const uint32_t count = static_cast<uint32_t>(slot.light_levels) * MICROPIXEL_GRAPHICS_RASTER_PALETTE_ENTRIES;
        for (uint32_t entry = 0U; entry < count; ++entry) {
            entries[entry] = static_cast<uint16_t>((entries[entry] << 8U) | (entries[entry] >> 8U));
        }
        slot.byte_swapped = byte_swapped;
    }
}

raster::Resources RasterService::ResourcesView() const {
    return raster::Resources{
        .textures = textures_.entries,
        .texture_count = textures_.capacity,
        .palettes = palettes_.entries,
        .palette_count = palettes_.capacity,
        .warps = warps_.entries,
        .warp_count = warps_.capacity,
    };
}

ServiceResult<void> RasterService::Submit(const uint8_t* bytes, uint32_t length, const DirectSurfaceService& surfaces,
                                          raster::TextureResolver resolve, void* context) {
    if (!available()) {
        return FailService<void>(MICROPIXEL_STATUS_UNSUPPORTED);
    }
    if (bytes == nullptr || length < sizeof(micropixel_raster_header_t)) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    raster::Resources resources = ResourcesView();
    resources.resolve_texture = resolve;
    resources.texture_context = context;
    micropixel_raster_header_t header{};
    std::memcpy(&header, bytes, sizeof(header));
    HostBufferView view{};
    const int32_t buffer_status = surfaces.HostBuffer(header.surface_handle, header.buffer_index, view);
    if (buffer_status != MICROPIXEL_STATUS_OK) {
        return FailService<void>(buffer_status);
    }
    const raster::Target target{
        .pixels = view.pixels,
        .width = view.width,
        .height = view.height,
        .pitch = view.pitch,
        .byte_swapped = surfaces.native_byte_swapped(),
    };
    const int32_t status = raster::ValidateDrawList(bytes, length, target, resources, header);
    if (status != MICROPIXEL_STATUS_OK) return FailService<void>(status);
    MatchPaletteByteOrder(surfaces.native_byte_swapped());
#if CONFIG_MICROPIXEL_APP_SURFACE_TELEMETRY_LOG
    // Two short lines every kTelemetrySubmits submissions: kernel time per
    // submit is the number to compare against the Guest's render_avg; the
    // per-kind line shows where that time goes (us per submit and ns per
    // requested pixel), so a kernel change can be judged by its own kind.
    telemetry_profile_.now_us = [] { return static_cast<uint64_t>(esp_timer_get_time()); };
    const int64_t started_us = esp_timer_get_time();
    raster::ExecuteDrawList(bytes, header, target, resources, &telemetry_profile_);
    telemetry_execute_us_ += static_cast<uint64_t>(esp_timer_get_time() - started_us);
    telemetry_records_ += header.record_count;
    telemetry_bytes_ += length;
    if (++telemetry_submits_ == kTelemetrySubmits) {
        ESP_LOGI(kTag,
                 "raster: submits=%" PRIu32 " avg_us=%" PRIu32 " records/submit=%" PRIu32 " bytes/submit=%" PRIu32,
                 telemetry_submits_, static_cast<uint32_t>(telemetry_execute_us_ / telemetry_submits_),
                 telemetry_records_ / telemetry_submits_, telemetry_bytes_ / telemetry_submits_);
        const raster::ExecuteProfile& p = telemetry_profile_;
        auto per_submit_us = [&](uint32_t kind) { return static_cast<uint32_t>(p.time_us[kind] / telemetry_submits_); };
        auto per_pixel_ns = [&](uint32_t kind) {
            return p.pixels[kind] == 0U ? 0U : static_cast<uint32_t>(p.time_us[kind] * 1000U / p.pixels[kind]);
        };
        auto per_submit = [&](uint32_t kind) { return p.records[kind] / telemetry_submits_; };
        ESP_LOGI(kTag,
                 "raster kinds (records/submit us/submit ns/px): column %" PRIu32 " %" PRIu32 " %" PRIu32
                 " | span_pair %" PRIu32 " %" PRIu32 " %" PRIu32 " | sprite %" PRIu32 " %" PRIu32 " %" PRIu32
                 " | rect %" PRIu32 " %" PRIu32 " %" PRIu32 " | image %" PRIu32 " %" PRIu32 " %" PRIu32
                 " | warp %" PRIu32 " %" PRIu32 " %" PRIu32,
                 per_submit(MICROPIXEL_RASTER_RECORD_COLUMN), per_submit_us(MICROPIXEL_RASTER_RECORD_COLUMN),
                 per_pixel_ns(MICROPIXEL_RASTER_RECORD_COLUMN), per_submit(MICROPIXEL_RASTER_RECORD_SPAN_PAIR),
                 per_submit_us(MICROPIXEL_RASTER_RECORD_SPAN_PAIR), per_pixel_ns(MICROPIXEL_RASTER_RECORD_SPAN_PAIR),
                 per_submit(MICROPIXEL_RASTER_RECORD_SPRITE), per_submit_us(MICROPIXEL_RASTER_RECORD_SPRITE),
                 per_pixel_ns(MICROPIXEL_RASTER_RECORD_SPRITE), per_submit(MICROPIXEL_RASTER_RECORD_RECT),
                 per_submit_us(MICROPIXEL_RASTER_RECORD_RECT), per_pixel_ns(MICROPIXEL_RASTER_RECORD_RECT),
                 per_submit(MICROPIXEL_RASTER_RECORD_IMAGE), per_submit_us(MICROPIXEL_RASTER_RECORD_IMAGE),
                 per_pixel_ns(MICROPIXEL_RASTER_RECORD_IMAGE), per_submit(MICROPIXEL_RASTER_RECORD_WARP),
                 per_submit_us(MICROPIXEL_RASTER_RECORD_WARP), per_pixel_ns(MICROPIXEL_RASTER_RECORD_WARP));
        telemetry_submits_ = 0U;
        telemetry_records_ = 0U;
        telemetry_bytes_ = 0U;
        telemetry_execute_us_ = 0U;
        telemetry_profile_ = raster::ExecuteProfile{};
    }
#else
    raster::ExecuteDrawList(bytes, header, target, resources);
#endif
    return {};
}

void RasterService::Shutdown() {
    FreeSlots(textures_.entries, textures_.capacity, [](const raster::Texture& slot) { return slot.pixels; });
    FreeSlots(palettes_.entries, palettes_.capacity, [](const raster::Palette& slot) { return slot.entries; });
    FreeSlots(warps_.entries, warps_.capacity, [](const raster::WarpMap& slot) { return slot.entries; });
    textures_ = {};
    palettes_ = {};
    warps_ = {};
}

}  // namespace micropixel::runtime
