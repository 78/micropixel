#include "runtime/resources/resource_service.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/resources/bitmap_decoder.hpp"
#include "work/background_executor.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_resource";
// Cache-line alignment keeps DMA2D/PPA source windows and their cache
// maintenance on whole lines.
constexpr size_t kStagedAssetAlignment = 64U;

bool IsRawBitmapFormat(uint32_t format) {
    return format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888 || format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888 ||
           format == MICROPIXEL_BUNDLE_FORMAT_RAW_RGB565;
}

uint32_t AssetPixelFormat(uint32_t format) {
    if (format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888) {
        return MICROPIXEL_PIXEL_FORMAT_BGRA8888;
    }
    return format == MICROPIXEL_BUNDLE_FORMAT_RAW_RGB565 ? MICROPIXEL_PIXEL_FORMAT_RGB565
                                                         : MICROPIXEL_PIXEL_FORMAT_BGR888;
}
}  // namespace

ResourceService::ResourceService(const micropixel_aot_package_t& package, work::BackgroundExecutor& background_executor,
                                 device::GraphicsService& graphics)
    : package_(package),
      background_executor_(background_executor),
      graphics_(graphics),
      work_done_(xSemaphoreCreateBinary()) {
    const auto graphics_info = graphics_.GetInfo();
    if (graphics_info && graphics_info->pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565) {
        preferred_opaque_format_.store(MICROPIXEL_PIXEL_FORMAT_RGB565);
    }
}

ResourceService::~ResourceService() {
    Shutdown();
    if (work_done_ != nullptr) {
        vSemaphoreDelete(work_done_);
    }
}

bool ResourceService::valid() const {
    return work_done_ != nullptr && background_executor_.valid() && bitmaps_.valid();
}

micropixel_texture_info_t ResourceService::TextureInfo(micropixel_texture_handle_t texture_handle,
                                                       const device::BitmapView& view) const {
    micropixel_texture_info_t info{};
    info.size = sizeof(info);
    info.width = view.width;
    info.height = view.height;
    info.physical_width = view.width;
    info.physical_height = view.height;
    info.pixel_format = view.pixel_format;
    // Host-only bits (byte order) never reach the Guest.
    info.flags = view.flags & MICROPIXEL_TEXTURE_FLAG_DYNAMIC;
    info.texture_handle = texture_handle;
    return info;
}

ServiceResult<micropixel_texture_info_t> ResourceService::AddAsset(const micropixel_bundle_asset_view_t& asset) {
    const uint32_t pixel_format = AssetPixelFormat(asset.format);
    device::BitmapView view{asset.data, asset.size, asset.width, asset.height, asset.stride, pixel_format};
    // The section mapping is closed as soon as this returns, so the texture
    // owns a PSRAM copy of the pixels. That is also the fast path: compositor
    // reads of a flash-mapped source measured ~180 ns/px against ~23 ns/px
    // from PSRAM, and sustained flash traffic starves the DSI frame buffer.
    auto* pixels = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(kStagedAssetAlignment, asset.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        ESP_LOGE(kTag, "raw asset staging failed: PSRAM allocation of %" PRIu32 " bytes", asset.size);
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    std::memcpy(pixels, asset.data, asset.size);
    view.data = pixels;
    const micropixel_texture_handle_t texture = bitmaps_.Add(view, true);
    if (texture == 0U) {
        heap_caps_free(pixels);
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    return TextureInfo(texture, view);
}

ServiceResult<micropixel_texture_info_t> ResourceService::LoadTexture(uint32_t asset_id, uint32_t scale_numerator,
                                                                      uint32_t scale_denominator) {
    if (asset_id == 0U || scale_numerator == 0U || scale_denominator == 0U || scale_numerator > 4096U ||
        scale_denominator > 4096U || stopping_.load(std::memory_order_acquire)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    // The section is addressable only for the duration of this call: decoded
    // or copied pixels become the texture, the Bundle bytes are released.
    micropixel_bundle_asset_mapping_t section{};
    if (!micropixel_bundle_open_asset(&package_, asset_id, &section)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    const micropixel_bundle_asset_view_t asset = section.asset;
    const bool scaled = scale_numerator != scale_denominator;

    if (!scaled && IsRawBitmapFormat(asset.format)) {
        const int64_t started_us = esp_timer_get_time();
        auto result = AddAsset(asset);
        micropixel_close_asset_mapping(&section);
        ESP_LOGI(kTag, "loaded raw asset=%" PRIu32 " texture=%" PRIu32 " bytes=%" PRIu32 " elapsed=%" PRId64 " us",
                 asset_id, result ? result->texture_handle : 0U, asset.size, esp_timer_get_time() - started_us);
        if (result) {
            last_decode_failure_[0] = '\0';
        }
        return result;
    }

    // Guest service calls are serialized. This stack context remains valid
    // because the call waits for Process() to signal completion below.
    Work work{this, asset, scale_numerator, scale_denominator, asset_id};
    completed_texture_ = 0U;
    completed_status_ = MICROPIXEL_STATUS_INTERNAL;
    while (xSemaphoreTake(work_done_, 0U) == pdTRUE) {
    }
    if (!background_executor_.Submit(ProcessEntry, &work)) {
        micropixel_close_asset_mapping(&section);
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    ESP_LOGI(kTag, "loading asset=%" PRIu32 " format=%" PRIu32 " bytes=%" PRIu32 " scale=%" PRIu32 "/%" PRIu32,
             asset_id, asset.format, asset.size, scale_numerator, scale_denominator);
    const bool completed = xSemaphoreTake(work_done_, portMAX_DELAY) == pdTRUE;
    micropixel_close_asset_mapping(&section);
    if (!completed) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    if (completed_status_ != MICROPIXEL_STATUS_OK || completed_texture_ == 0U) {
        return FailService<micropixel_texture_info_t>(completed_status_);
    }
    device::BitmapView view{};
    if (!bitmaps_.Resolve(completed_texture_, view)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    micropixel_texture_info_t info = TextureInfo(completed_texture_, view);
    // The Guest addresses the authored size; only the stored bitmap is scaled.
    info.width = asset.width;
    info.height = asset.height;
    last_decode_failure_[0] = '\0';
    return info;
}

ServiceResult<void> ResourceService::ReleaseTexture(micropixel_texture_handle_t texture_handle) {
    if (texture_handle == 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    bitmaps_.Release(texture_handle);
    return {};
}

ServiceResult<device::FontResourceView> ResourceService::FindFont(uint32_t resource_id) {
    if (resource_id == 0U || stopping_.load(std::memory_order_acquire)) {
        return FailService<device::FontResourceView>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (fonts_ == nullptr) {
        fonts_ = static_cast<FontSlot*>(
            heap_caps_calloc(package_.section_count, sizeof(FontSlot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (fonts_ == nullptr) {
            ESP_LOGE(kTag, "font slot allocation failed: %" PRIu32 " slots", package_.section_count);
            return FailService<device::FontResourceView>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
    }
    for (uint32_t index = 0U; index < font_count_; ++index) {
        if (fonts_[index].resource_id == resource_id) {
            return device::FontResourceView{fonts_[index].mapping.font.data, fonts_[index].mapping.font.size};
        }
    }
    if (font_count_ >= package_.section_count) {
        return FailService<device::FontResourceView>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    FontSlot& slot = fonts_[font_count_];
    if (!micropixel_bundle_open_font(&package_, resource_id, &slot.mapping)) {
        return FailService<device::FontResourceView>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    slot.resource_id = resource_id;
    ++font_count_;
    return device::FontResourceView{slot.mapping.font.data, slot.mapping.font.size};
}

ServiceResult<micropixel_texture_info_t> ResourceService::CreateDynamicTexture(
    const micropixel_dynamic_texture_create_request_t& request) {
    if (stopping_.load(std::memory_order_acquire) || request.size != sizeof(request) || request.reserved0)
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    uint8_t* pixels = nullptr;
    if (request.pixels || request.length || request.pitch) {
        if (!request.pixels || !request.length || !memory_.resolve ||
            !memory_.resolve(memory_.context, request.pixels, request.length, &pixels) || !pixels)
            return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_MEMORY);
    }
    auto created = bitmaps_.CreateDynamic(request.width, request.height, request.pixel_format, pixels, request.length,
                                          request.pitch);
    if (!created) return FailService<micropixel_texture_info_t>(created.error().status);
    device::BitmapView view{};
    if (!bitmaps_.Resolve(*created, view)) {
        bitmaps_.Release(*created);
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    return TextureInfo(*created, view);
}

ServiceResult<micropixel_texture_info_t> ResourceService::UpdateDynamicTexture(
    const micropixel_dynamic_texture_update_request_t& request) {
    if (stopping_.load(std::memory_order_acquire) || request.size != sizeof(request) || request.reserved0)
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    uint8_t* pixels = nullptr;
    if (!request.pixels || !request.length || !memory_.resolve ||
        !memory_.resolve(memory_.context, request.pixels, request.length, &pixels) || !pixels)
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_MEMORY);
    auto created = bitmaps_.UpdateDynamic(request.texture_handle, request.x, request.y, request.width, request.height,
                                          pixels, request.length, request.pitch);
    if (!created) return FailService<micropixel_texture_info_t>(created.error().status);
    device::BitmapView view{};
    if (!bitmaps_.Resolve(*created, view)) {
        bitmaps_.Release(*created);
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    return TextureInfo(*created, view);
}

bool ResourceService::ResolveTexture(micropixel_texture_handle_t texture_handle, device::BitmapView& view_out) const {
    return bitmaps_.Resolve(texture_handle, view_out);
}

bool ResourceService::RetainSceneTexture(micropixel_texture_handle_t texture_handle) {
    return bitmaps_.RetainSceneReference(texture_handle);
}

void ResourceService::ReleaseSceneTexture(micropixel_texture_handle_t texture_handle) {
    bitmaps_.ReleaseSceneReference(texture_handle);
}

void ResourceService::ProcessEntry(void* argument) {
    auto* work = static_cast<Work*>(argument);
    if (work != nullptr && work->service != nullptr) {
        work->service->Process(*work);
    }
}

void ResourceService::Process(const Work& work) {
    const int64_t started = esp_timer_get_time();
    micropixel_texture_handle_t texture = 0U;
    const int32_t load_status = LoadOwnedAsset(work, texture);
    completed_texture_ = texture;
    completed_status_ = load_status;
    device::BitmapView loaded{};
    (void)bitmaps_.Resolve(texture, loaded);
    ESP_LOGI(kTag,
             "loaded format=%" PRIu32 " scale=%" PRIu32 "/%" PRIu32 " texture=%" PRIu32 " %" PRIu32 "x%" PRIu32
             " bytes=%" PRIu32 " elapsed=%" PRId64 " us status=%" PRId32,
             work.asset.format, work.scale_numerator, work.scale_denominator, texture, loaded.width, loaded.height,
             loaded.size, esp_timer_get_time() - started, completed_status_);
    xSemaphoreGive(work_done_);
}

int32_t ResourceService::LoadOwnedAsset(const Work& work, micropixel_texture_handle_t& texture_out) {
    DecodedBitmap source{};
    const uint32_t pixel_format = AssetPixelFormat(work.asset.format);
    const bool raw = IsRawBitmapFormat(work.asset.format);
    if (raw) {
        if (!AllocateBitmap(work.asset.width, work.asset.height, pixel_format, source)) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        auto* destination = const_cast<uint8_t*>(source.view().data);
        const uint32_t row_bytes = source.view().stride;
        if (work.asset.stride < row_bytes) {
            return MICROPIXEL_STATUS_INTERNAL;
        }
        for (uint32_t row = 0U; row < work.asset.height; ++row) {
            std::memcpy(destination + row * row_bytes, work.asset.data + row * work.asset.stride, row_bytes);
        }
    } else if (!DecodeBitmap(work.asset, preferred_opaque_format_.load(), source)) {
        (void)std::snprintf(last_decode_failure_.data(), last_decode_failure_.size(), "asset=%" PRIu32 ": %s",
                            work.asset_id, source.FailureDetail());
        return MICROPIXEL_STATUS_INTERNAL;
    }

    DecodedBitmap scaled{};
    DecodedBitmap* output = &source;
    if (work.scale_numerator != work.scale_denominator) {
        const auto scaled_dimension = [&work](uint32_t value) {
            return static_cast<uint32_t>(
                (static_cast<uint64_t>(value) * work.scale_numerator + work.scale_denominator / 2U) /
                work.scale_denominator);
        };
        const uint32_t width = scaled_dimension(source.view().width);
        const uint32_t height = scaled_dimension(source.view().height);
        constexpr uint32_t kPpaStrideAlignmentPixels = 32U;
        if (width == 0U || height == 0U ||
            !AllocateBitmap(width, height, source.view().pixel_format, scaled, kPpaStrideAlignmentPixels)) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        auto scale_result = graphics_.ScaleBitmap(source.view(), scaled.view());
        if (!scale_result) {
            return scale_result.error().status;
        }
        output = &scaled;
    }

    const bool rgb565 = output->view().pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565;
    texture_out = bitmaps_.Add(output->view(), true);
    if (texture_out == 0U) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    output->ReleaseOwnership();
    // Decoders and the scaler produce canonical RGB565; a raster target in
    // panel order wants the texture the same way so IMAGE copies move bytes
    // verbatim (and may run on a DMA engine).
    if (rgb565 && preferred_rgb565_swapped_.load()) {
        (void)bitmaps_.SetRgb565ByteOrder(texture_out, true);
    }
    return MICROPIXEL_STATUS_OK;
}

bool ResourceService::ResolveTextureForRaster(micropixel_texture_handle_t texture_handle, bool target_byte_swapped,
                                              device::BitmapView& view_out) {
    if (!bitmaps_.Resolve(texture_handle, view_out)) {
        return false;
    }
    const bool swapped = (view_out.flags & device::bitmap_flags::kRgb565ByteSwapped) != 0U;
    if (view_out.pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565 && swapped != target_byte_swapped &&
        bitmaps_.SetRgb565ByteOrder(texture_handle, target_byte_swapped)) {
        return bitmaps_.Resolve(texture_handle, view_out);
    }
    return true;
}

void ResourceService::Shutdown() {
    if (shutdown_complete_) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    const uint32_t texture_high_water_mark = bitmaps_.HighWaterMark();
    bitmaps_.ReleaseAll();
    // Guest fonts were released with the graphics resources before this.
    for (uint32_t index = 0U; index < font_count_; ++index) {
        micropixel_close_font_mapping(&fonts_[index].mapping);
    }
    heap_caps_free(fonts_);
    fonts_ = nullptr;
    font_count_ = 0U;
    shutdown_complete_ = true;
    ESP_LOGI(kTag, "resource textures released: high-water=%" PRIu32 "/%" PRIu32, texture_high_water_mark,
             limits::kMaxBitmaps);
}

}  // namespace micropixel::runtime
