#include "runtime/resources/resource_service.hpp"

#include <cinttypes>

#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/resources/bitmap_decoder.hpp"
#include "work/background_executor.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_resource";
}  // namespace

ResourceService::ResourceService(const micropixel_aot_package_t& package, work::BackgroundExecutor& background_executor)
    : package_(package), background_executor_(background_executor), work_done_(xSemaphoreCreateBinary()) {}

ResourceService::~ResourceService() {
    Shutdown();
    if (work_done_ != nullptr) {
        vSemaphoreDelete(work_done_);
    }
}

bool ResourceService::valid() const { return work_done_ != nullptr && background_executor_.valid(); }

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
    const uint32_t pixel_format = asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888
                                      ? MICROPIXEL_PIXEL_FORMAT_BGRA8888
                                      : MICROPIXEL_PIXEL_FORMAT_BGR888;
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

    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888 || asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888) {
        auto result = AddAsset(asset);
        ESP_LOGI(kTag, "loaded raw asset=%" PRIu32 " texture=%" PRIu32 " flash=%p", asset_id,
                 result ? result->texture : 0U, asset.data);
        return result;
    }

    // Guest service calls are serialized. This stack context remains valid
    // because the call waits for Process() to signal completion below.
    Work work{this, asset};
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
        width > CONFIG_MICROPIXEL_MAX_TEXTURE_DIMENSION || height > CONFIG_MICROPIXEL_MAX_TEXTURE_DIMENSION ||
        (pixel_format != MICROPIXEL_PIXEL_FORMAT_BGR888 && pixel_format != MICROPIXEL_PIXEL_FORMAT_BGRA8888)) {
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
    DecodedBitmap decoded{};
    const bool decoded_ok = DecodeBitmap(work.asset, decoded);
    micropixel_texture_handle_t texture = 0U;
    if (decoded_ok) {
        texture = bitmaps_.Add(decoded.view(), true);
        if (texture != 0U) {
            decoded.ReleaseOwnership();
        }
    }
    completed_texture_ = texture;
    completed_status_ = texture != 0U
                            ? MICROPIXEL_STATUS_OK
                            : (decoded_ok ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED : MICROPIXEL_STATUS_INTERNAL);
    ESP_LOGI(kTag,
             "decoded format=%" PRIu32 " texture=%" PRIu32 " %" PRIu32 "x%" PRIu32 " bytes=%" PRIu32 " elapsed=%" PRId64
             " us status=%" PRId32,
             work.asset.format, texture, decoded.view().width, decoded.view().height, decoded.view().size,
             esp_timer_get_time() - started, completed_status_);
    xSemaphoreGive(work_done_);
}

void ResourceService::Shutdown() {
    if (shutdown_complete_) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    bitmaps_.ReleaseAll();
    shutdown_complete_ = true;
    ESP_LOGI(kTag, "resource textures released");
}

}  // namespace micropixel::runtime
