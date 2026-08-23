#include "runtime/resources/resource_service.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/resources/bitmap_decoder.hpp"
#include "task_policy.hpp"

namespace micropixel::runtime {
namespace {

constexpr char kTag[] = "micropixel_resource";
constexpr uint32_t kWorkerStackBytes = 12U * 1024U;
constexpr BaseType_t kWorkerCore = 0;

}  // namespace

ResourceService::ResourceService(const micropixel_aot_package_t& package, EventQueue& events, int64_t clock_origin_us)
    : package_(package),
      events_(events),
      clock_origin_us_(clock_origin_us),
      work_queue_(xQueueCreate(limits::kMaxResourceRequests, sizeof(Work))),
      worker_stopped_(xSemaphoreCreateBinary()) {
    if (work_queue_ == nullptr || worker_stopped_ == nullptr) {
        return;
    }
    if (xTaskCreatePinnedToCore(WorkerEntry, "micropixel_assets", kWorkerStackBytes, this,
                                task_policy::kAssetWorkerPriority, &worker_, kWorkerCore) != pdPASS) {
        worker_ = nullptr;
    }
}

ResourceService::~ResourceService() {
    Shutdown();
    if (work_queue_ != nullptr) {
        vQueueDelete(work_queue_);
    }
    if (worker_stopped_ != nullptr) {
        vSemaphoreDelete(worker_stopped_);
    }
}

bool ResourceService::valid() const {
    return work_queue_ != nullptr && worker_stopped_ != nullptr && worker_ != nullptr;
}

uint32_t ResourceService::AllocateRequestLocked() {
    for (auto& slot : requests_) {
        if (slot.handle == 0U) {
            uint32_t handle = next_request_++;
            if (handle == 0U) {
                handle = next_request_++;
            }
            slot.handle = handle;
            return handle;
        }
    }
    return 0U;
}

bool ResourceService::RequestPendingLocked(uint32_t request) const {
    for (const auto& slot : requests_) {
        if (slot.handle == request) {
            return true;
        }
    }
    return false;
}

void ResourceService::ClearRequestLocked(uint32_t request) {
    for (auto& slot : requests_) {
        if (slot.handle == request) {
            slot.handle = 0U;
            return;
        }
    }
}

ServiceResult<micropixel_load_request_handle_t> ResourceService::Load(uint32_t asset_id) {
    if (asset_id == 0U || stopping_.load(std::memory_order_acquire)) {
        return FailService<micropixel_load_request_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_bundle_asset_view_t asset{};
    if (!micropixel_bundle_find_asset(&package_, asset_id, &asset)) {
        return FailService<micropixel_load_request_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    portENTER_CRITICAL(&request_lock_);
    uint32_t request = AllocateRequestLocked();
    portEXIT_CRITICAL(&request_lock_);
    if (request == 0U) {
        return FailService<micropixel_load_request_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }

    if (asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_RGB888 || asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_ARGB8888) {
        const uint32_t pixel_format = asset.format == MICROPIXEL_BUNDLE_FORMAT_RAW_ARGB8888
                                          ? MICROPIXEL_PIXEL_FORMAT_ARGB8888
                                          : MICROPIXEL_PIXEL_FORMAT_RGB888;
        device::BitmapView view{asset.data, asset.size, asset.width, asset.height, asset.stride, pixel_format};
        uint32_t bitmap = bitmaps_.Add(view, false);
        int32_t status = bitmap != 0U ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        PostCompletion(request, bitmap, status);
        ESP_LOGI(kTag, "raw asset=%" PRIu32 " request=%" PRIu32 " completed inline bitmap=%" PRIu32 " flash=%p",
                 asset_id, request, bitmap, asset.data);
        return request;
    }

    Work work{request, asset};
    if (xQueueSend(work_queue_, &work, 0) != pdTRUE) {
        portENTER_CRITICAL(&request_lock_);
        ClearRequestLocked(request);
        portEXIT_CRITICAL(&request_lock_);
        return FailService<micropixel_load_request_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    ESP_LOGI(kTag, "queued compressed asset=%" PRIu32 " request=%" PRIu32 " format=%" PRIu32 " bytes=%" PRIu32,
             asset_id, request, asset.format, asset.size);
    return request;
}

ServiceResult<void> ResourceService::Cancel(micropixel_load_request_handle_t request) {
    if (request == 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    portENTER_CRITICAL(&request_lock_);
    ClearRequestLocked(request);
    portEXIT_CRITICAL(&request_lock_);
    return {};
}

ServiceResult<micropixel_bitmap_info_t> ResourceService::BitmapInfo(micropixel_bitmap_handle_t bitmap) {
    if (bitmap == 0U) {
        return FailService<micropixel_bitmap_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    device::BitmapView view{};
    if (!ResolveBitmap(bitmap, view)) {
        return FailService<micropixel_bitmap_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_bitmap_info_t info{};
    info.size = sizeof(info);
    info.interface_major = 1U;
    info.interface_minor = 0U;
    info.width = view.width;
    info.height = view.height;
    info.stride = view.stride;
    info.pixel_format = view.pixel_format;
    info.flags = view.flags;
    return info;
}

ServiceResult<void> ResourceService::BitmapRelease(micropixel_bitmap_handle_t bitmap) {
    if (bitmap == 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    bitmaps_.Release(bitmap);
    return {};
}

ServiceResult<micropixel_bitmap_handle_t> ResourceService::CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                                                  uint32_t pixel_format) {
    if (stopping_.load(std::memory_order_acquire) || width == 0U || height == 0U || width > 720U || height > 720U ||
        (pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB888 && pixel_format != MICROPIXEL_PIXEL_FORMAT_ARGB8888)) {
        return FailService<micropixel_bitmap_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const micropixel_bitmap_handle_t bitmap = bitmaps_.CreateOffscreenSurface(width, height, pixel_format);
    if (bitmap == 0U) {
        return FailService<micropixel_bitmap_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    ESP_LOGI(kTag, "created offscreen surface bitmap=%" PRIu32 " %" PRIu32 "x%" PRIu32 " format=%" PRIu32, bitmap,
             width, height, pixel_format);
    return bitmap;
}

ServiceResult<device::BitmapView> ResourceService::MutableBitmap(micropixel_bitmap_handle_t bitmap) const {
    if (bitmap == 0U) {
        return FailService<device::BitmapView>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    device::BitmapView view{};
    if (!bitmaps_.ResolveMutable(bitmap, view)) {
        return FailService<device::BitmapView>(MICROPIXEL_STATUS_PERMISSION_DENIED);
    }
    return view;
}

bool ResourceService::ResolveBitmap(micropixel_bitmap_handle_t bitmap, device::BitmapView& view_out) const {
    return bitmaps_.Resolve(bitmap, view_out);
}

void ResourceService::PostCompletion(uint32_t request, uint32_t bitmap, int32_t status) {
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_RESOURCE_EVENT_READY;
    event.service_id = MICROPIXEL_SERVICE_RESOURCE;
    int64_t now = esp_timer_get_time();
    event.timestamp_us = now >= clock_origin_us_ ? static_cast<uint64_t>(now - clock_origin_us_) : 0U;
    event.source = request;
    event.status = status;
    micropixel_resource_event_payload_t payload{};
    payload.bitmap = bitmap;
    std::memcpy(event.payload, &payload, sizeof(payload));
    if (!events_.PushRequired(event) && bitmap != 0U) {
        (void)BitmapRelease(bitmap);
    }
}

void ResourceService::WorkerEntry(void* argument) { static_cast<ResourceService*>(argument)->WorkerLoop(); }

void ResourceService::WorkerLoop() {
    while (!stopping_.load(std::memory_order_acquire)) {
        Work work{};
        if (xQueueReceive(work_queue_, &work, pdMS_TO_TICKS(20)) == pdTRUE && work.request != 0U) {
            Process(work);
        }
    }
    xSemaphoreGive(worker_stopped_);
    vTaskDelete(nullptr);
}

void ResourceService::Process(const Work& work) {
    int64_t started = esp_timer_get_time();
    DecodedBitmap decoded{};
    bool decoded_ok = DecodeBitmap(work.asset, decoded);

    uint32_t bitmap = 0U;
    bool pending = false;
    portENTER_CRITICAL(&request_lock_);
    pending = RequestPendingLocked(work.request);
    ClearRequestLocked(work.request);
    portEXIT_CRITICAL(&request_lock_);

    if (pending && decoded_ok) {
        bitmap = bitmaps_.Add(decoded.view(), true);
        if (bitmap != 0U) {
            decoded.ReleaseOwnership();
        }
    }
    if (pending) {
        int32_t status = bitmap != 0U
                             ? MICROPIXEL_STATUS_OK
                             : (decoded_ok ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED : MICROPIXEL_STATUS_INTERNAL);
        PostCompletion(work.request, bitmap, status);
        ESP_LOGI(kTag,
                 "decoded request=%" PRIu32 " format=%" PRIu32 " bitmap=%" PRIu32 " %" PRIu32 "x%" PRIu32
                 " bytes=%" PRIu32 " elapsed=%" PRId64 " us status=%" PRId32,
                 work.request, work.asset.format, bitmap, decoded.view().width, decoded.view().height,
                 decoded.view().size, esp_timer_get_time() - started, status);
    }
}

void ResourceService::Shutdown() {
    if (shutdown_complete_) {
        return;
    }
    stopping_.store(true, std::memory_order_release);
    if (worker_ != nullptr && worker_stopped_ != nullptr) {
        (void)xSemaphoreTake(worker_stopped_, pdMS_TO_TICKS(2000));
        worker_ = nullptr;
    }
    bitmaps_.ReleaseAll();
    shutdown_complete_ = true;
    ESP_LOGI(kTag, "resource worker stopped; requests canceled and bitmaps released");
}

}  // namespace micropixel::runtime
