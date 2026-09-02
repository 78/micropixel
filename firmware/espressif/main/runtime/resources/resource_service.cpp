#include "runtime/resources/resource_service.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/resources/bitmap_decoder.hpp"
#include "work/background_executor.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_resource";

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
        preferred_opaque_format_ = MICROPIXEL_PIXEL_FORMAT_RGB565;
    }
}

ResourceService::~ResourceService() {
    Shutdown();
    if (work_done_ != nullptr) {
        vSemaphoreDelete(work_done_);
    }
}

bool ResourceService::valid() const { return work_done_ != nullptr && background_executor_.valid() && bitmaps_.valid(); }

micropixel_texture_info_t ResourceService::TextureInfo(micropixel_texture_handle_t texture,
                                                       const device::BitmapView& view) const {
    micropixel_texture_info_t info{};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_RESOURCE_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_RESOURCE_INTERFACE_MINOR;
    info.width = view.width;
    info.height = view.height;
    info.stride = view.stride;
    info.pixel_format = view.pixel_format;
    info.flags = view.flags;
    info.texture = texture;
    return info;
}

ServiceResult<micropixel_texture_info_t> ResourceService::AddAsset(const micropixel_bundle_asset_view_t& asset) {
    const uint32_t pixel_format = AssetPixelFormat(asset.format);
    device::BitmapView view{asset.data, asset.size, asset.width, asset.height, asset.stride, pixel_format};
    const micropixel_texture_handle_t texture = bitmaps_.Add(view, false);
    if (texture == 0U) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    return TextureInfo(texture, view);
}

ServiceResult<micropixel_texture_info_t> ResourceService::LoadTexture(uint32_t asset_id) {
    if (asset_id == 0U || stopping_.load(std::memory_order_acquire)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_bundle_asset_view_t asset{};
    if (!micropixel_bundle_find_asset(&package_, asset_id, &asset)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }

    if (IsRawBitmapFormat(asset.format)) {
        auto result = AddAsset(asset);
        ESP_LOGI(kTag, "loaded raw asset=%" PRIu32 " texture=%" PRIu32 " flash=%p", asset_id,
                 result ? result->texture : 0U, asset.data);
        return result;
    }

    // Guest service calls are serialized. This stack context remains valid
    // because the call waits for Process() to signal completion below.
    Work work{this, asset, false, 1U, 1U};
    completed_texture_ = 0U;
    completed_status_ = MICROPIXEL_STATUS_INTERNAL;
    while (xSemaphoreTake(work_done_, 0U) == pdTRUE) {
    }
    if (!background_executor_.Submit(ProcessEntry, &work)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    ESP_LOGI(kTag, "loading compressed asset=%" PRIu32 " format=%" PRIu32 " bytes=%" PRIu32, asset_id, asset.format,
             asset.size);
    if (xSemaphoreTake(work_done_, portMAX_DELAY) != pdTRUE) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    if (completed_status_ != MICROPIXEL_STATUS_OK || completed_texture_ == 0U) {
        return FailService<micropixel_texture_info_t>(completed_status_);
    }
    device::BitmapView view{};
    if (!bitmaps_.Resolve(completed_texture_, view)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    return TextureInfo(completed_texture_, view);
}

ServiceResult<micropixel_adaptive_texture_info_t> ResourceService::LoadAdaptiveTexture(uint32_t asset_id,
                                                                                       uint32_t scale_numerator,
                                                                                       uint32_t scale_denominator) {
    if (asset_id == 0U || scale_numerator == 0U || scale_denominator == 0U || scale_numerator > 4096U ||
        scale_denominator > 4096U || stopping_.load(std::memory_order_acquire)) {
        return FailService<micropixel_adaptive_texture_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_bundle_asset_view_t asset{};
    if (!micropixel_bundle_find_asset(&package_, asset_id, &asset)) {
        return FailService<micropixel_adaptive_texture_info_t>(MICROPIXEL_STATUS_NOT_FOUND);
    }

    const bool raw = IsRawBitmapFormat(asset.format);
    if (scale_numerator == scale_denominator && raw) {
        auto loaded = AddAsset(asset);
        if (!loaded) {
            return FailService<micropixel_adaptive_texture_info_t>(loaded.error().status);
        }
        micropixel_adaptive_texture_info_t info{};
        info.size = sizeof(info);
        info.interface_major = MICROPIXEL_RESOURCE_INTERFACE_MAJOR;
        info.interface_minor = MICROPIXEL_RESOURCE_INTERFACE_MINOR;
        info.logical_width = asset.width;
        info.logical_height = asset.height;
        info.physical_width = loaded->width;
        info.physical_height = loaded->height;
        info.stride = loaded->stride;
        info.pixel_format = loaded->pixel_format;
        info.flags = loaded->flags;
        info.texture = loaded->texture;
        return info;
    }

    completed_texture_ = 0U;
    completed_status_ = MICROPIXEL_STATUS_INTERNAL;
    while (xSemaphoreTake(work_done_, 0U) == pdTRUE) {
    }
    Work work{this, asset, true, scale_numerator, scale_denominator};
    if (!background_executor_.Submit(ProcessEntry, &work)) {
        return FailService<micropixel_adaptive_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    if (xSemaphoreTake(work_done_, portMAX_DELAY) != pdTRUE || completed_status_ != MICROPIXEL_STATUS_OK ||
        completed_texture_ == 0U) {
        return FailService<micropixel_adaptive_texture_info_t>(completed_status_);
    }
    device::BitmapView view{};
    if (!bitmaps_.Resolve(completed_texture_, view)) {
        return FailService<micropixel_adaptive_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    micropixel_adaptive_texture_info_t info{};
    info.size = sizeof(info);
    info.interface_major = MICROPIXEL_RESOURCE_INTERFACE_MAJOR;
    info.interface_minor = MICROPIXEL_RESOURCE_INTERFACE_MINOR;
    info.logical_width = asset.width;
    info.logical_height = asset.height;
    info.physical_width = view.width;
    info.physical_height = view.height;
    info.stride = view.stride;
    info.pixel_format = view.pixel_format;
    info.flags = view.flags;
    info.texture = completed_texture_;
    return info;
}

ServiceResult<void> ResourceService::ReleaseTexture(micropixel_texture_handle_t texture) {
    if (texture == 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    bitmaps_.Release(texture);
    return {};
}

ServiceResult<device::FontResourceView> ResourceService::FindFont(uint32_t resource_id) const {
    if (resource_id == 0U || stopping_.load(std::memory_order_acquire)) {
        return FailService<device::FontResourceView>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_bundle_font_view_t font{};
    if (!micropixel_bundle_find_font(&package_, resource_id, &font)) {
        return FailService<device::FontResourceView>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    return device::FontResourceView{font.data, font.size};
}

ServiceResult<micropixel_texture_info_t> ResourceService::CreateStreamingTexture(uint32_t width, uint32_t height,
                                                                                 uint32_t pixel_format) {
    if (stopping_.load(std::memory_order_acquire) || width == 0U || height == 0U ||
        (pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 && pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888 &&
         pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565)) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const micropixel_texture_handle_t texture = bitmaps_.CreateOffscreenSurface(width, height, pixel_format);
    if (texture == 0U) {
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    device::BitmapView view{};
    if (!bitmaps_.Resolve(texture, view)) {
        bitmaps_.Release(texture);
        return FailService<micropixel_texture_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    ESP_LOGI(kTag, "created streaming texture=%" PRIu32 " %" PRIu32 "x%" PRIu32 " format=%" PRIu32, texture, width,
             height, pixel_format);
    return TextureInfo(texture, view);
}

ServiceResult<device::BitmapView> ResourceService::MutableTexture(micropixel_texture_handle_t texture) const {
    if (texture == 0U) {
        return FailService<device::BitmapView>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    device::BitmapView view{};
    if (!bitmaps_.ResolveMutable(texture, view)) {
        /* An immutable or stale Texture is an invalid method argument. Keep
         * PermissionDenied reserved for App authorization policy. */
        return FailService<device::BitmapView>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return view;
}

bool ResourceService::ResolveTexture(micropixel_texture_handle_t texture, device::BitmapView& view_out) const {
    return bitmaps_.Resolve(texture, view_out);
}

bool ResourceService::RetainSceneTexture(micropixel_texture_handle_t texture) {
    return bitmaps_.RetainSceneReference(texture);
}

void ResourceService::ReleaseSceneTexture(micropixel_texture_handle_t texture) {
    bitmaps_.ReleaseSceneReference(texture);
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
             "loaded format=%" PRIu32 " adaptive=%d texture=%" PRIu32 " %" PRIu32 "x%" PRIu32 " bytes=%" PRIu32
             " elapsed=%" PRId64 " us status=%" PRId32,
             work.asset.format, work.adaptive, texture, loaded.width, loaded.height, loaded.size,
             esp_timer_get_time() - started, completed_status_);
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
    } else if (!DecodeBitmap(work.asset, preferred_opaque_format_, source)) {
        return MICROPIXEL_STATUS_INTERNAL;
    }

    DecodedBitmap scaled{};
    DecodedBitmap* output = &source;
    if (work.adaptive && work.scale_numerator != work.scale_denominator) {
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

    texture_out = bitmaps_.Add(output->view(), true);
    if (texture_out == 0U) {
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    output->ReleaseOwnership();
    return MICROPIXEL_STATUS_OK;
}

void ResourceService::Shutdown() {
    if (shutdown_complete_) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    const uint32_t texture_high_water_mark = bitmaps_.HighWaterMark();
    bitmaps_.ReleaseAll();
    shutdown_complete_ = true;
    ESP_LOGI(kTag, "resource textures released: high-water=%" PRIu32 "/%" PRIu32, texture_high_water_mark,
             limits::kMaxBitmaps);
}

}  // namespace micropixel::runtime
