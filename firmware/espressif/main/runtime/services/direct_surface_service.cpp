#include "runtime/services/direct_surface_service.hpp"

#include <cinttypes>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace micropixel::runtime {
namespace {

constexpr const char* kTag = "direct_surface_svc";
constexpr uint32_t kBytesPerRgb565 = 2U;

}  // namespace

DirectSurfaceService::DirectSurfaceService(device::GraphicsService& graphics, EventQueue& events,
                                           int64_t clock_origin_us)
    : graphics_(graphics), events_(events), clock_origin_us_(clock_origin_us) {}

DirectSurfaceService::~DirectSurfaceService() { Shutdown(); }

bool DirectSurfaceService::AllocateHostBuffers(uint32_t width, uint32_t height, uint32_t count) {
    const uint64_t pitch = static_cast<uint64_t>(width) * kBytesPerRgb565;
    const uint64_t bytes = pitch * height;
    if (bytes == 0U || bytes > UINT32_MAX) {
        return false;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        // Same placement and alignment as a Guest buffer would have (PSRAM,
        // 64 B) so the presenter's DMA and cache handling stay identical.
        host_pixels_[index] = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            MICROPIXEL_SURFACE_BUFFER_ALIGNMENT, static_cast<size_t>(bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (host_pixels_[index] == nullptr) {
            ESP_LOGW(kTag, "Host buffer %" PRIu32 " of %" PRIu32 " (%" PRIu32 " B) allocation failed", index, count,
                     static_cast<uint32_t>(bytes));
            FreeHostBuffers();
            return false;
        }
        std::memset(host_pixels_[index], 0, static_cast<size_t>(bytes));
    }
    host_width_ = width;
    host_height_ = height;
    host_pitch_ = static_cast<uint32_t>(pitch);
    host_buffers_ = true;
    return true;
}

void DirectSurfaceService::FreeHostBuffers() {
    for (uint8_t*& pixels : host_pixels_) {
        heap_caps_free(pixels);
        pixels = nullptr;
    }
    host_width_ = 0U;
    host_height_ = 0U;
    host_pitch_ = 0U;
    host_buffers_ = false;
}

ServiceResult<micropixel_surface_create_response_t> DirectSurfaceService::Create(
    const micropixel_surface_create_request_t& request) {
    if (request.size != sizeof(request) || request.reserved0 != 0U || request.width == 0U || request.height == 0U ||
        request.pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565 || request.buffer_count == 0U ||
        request.buffer_count > MICROPIXEL_SURFACE_MAX_BUFFERS ||
        (request.flags & ~MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS) != 0U) {
        return FailService<micropixel_surface_create_response_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (created_.load(std::memory_order_acquire)) {
        return FailService<micropixel_surface_create_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const bool host_buffers = (request.flags & MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS) == 0U;
    if (!host_buffers) {
        if (memory_.resolve == nullptr) {
            return FailService<micropixel_surface_create_response_t>(MICROPIXEL_STATUS_UNSUPPORTED);
        }
        if (!memory_.stable_base) {
            // Not a device limitation but a Bundle one; say so, or the App
            // author will look for a missing panel feature.
            ESP_LOGW(kTag,
                     "SURFACE_CREATE refused: Bundle must declare pinned_memory (app.json) so Guest buffer "
                     "addresses stay valid while frames are in flight, or use Host buffers (default)");
            return FailService<micropixel_surface_create_response_t>(MICROPIXEL_STATUS_UNSUPPORTED);
        }
    } else if (!AllocateHostBuffers(request.width, request.height, request.buffer_count)) {
        return FailService<micropixel_surface_create_response_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    // The device sees plain buffers either way; ownership is this service's concern.
    const device::DirectSurfaceConfig config{
        .width = request.width,
        .height = request.height,
        .pixel_format = request.pixel_format,
        .buffer_count = request.buffer_count,
        .flags = 0U,
    };
    // Publish the handle before the device can release anything.
    retiring_.store(false, std::memory_order_release);
    handle_ = next_handle_++;
    if (next_handle_ == 0U) {
        next_handle_ = 1U;
    }
    buffer_count_ = request.buffer_count;
    created_.store(true, std::memory_order_release);
    auto result = graphics_.CreateDirectSurface(config, {.context = this, .release = OnBufferReleased});
    if (!result) {
        created_.store(false, std::memory_order_release);
        FreeHostBuffers();
        return FailService<micropixel_surface_create_response_t>(result.error().status);
    }
    micropixel_surface_create_response_t response{};
    response.size = sizeof(response);
    response.surface = handle_;
    response.native_pixel_format = result->native_pixel_format;
    response.native_flags = result->native_flags;
    response.max_full_frame_fps = result->max_full_frame_fps;
    native_byte_swapped_ = (result->native_flags & MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED) != 0U;
    ESP_LOGI(kTag,
             "surface %" PRIu32 " created: %" PRIu32 "x%" PRIu32 " buffers=%" PRIu32 " (%s) native_flags=0x%" PRIx32,
             handle_, request.width, request.height, request.buffer_count, host_buffers ? "Host PSRAM" : "Guest memory",
             response.native_flags);
    return response;
}

int32_t DirectSurfaceService::HostBuffer(uint32_t index, HostBufferView& view_out) const {
    if (!created_.load(std::memory_order_acquire) || !host_buffers_ || index >= buffer_count_) {
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if ((in_flight_mask_.load(std::memory_order_acquire) & (1U << index)) != 0U) {
        return MICROPIXEL_STATUS_STALE_STATE;
    }
    view_out = {.pixels = host_pixels_[index], .width = host_width_, .height = host_height_, .pitch = host_pitch_};
    return MICROPIXEL_STATUS_OK;
}

ServiceResult<void> DirectSurfaceService::Present(const micropixel_surface_present_request_t& request) {
    if (request.size != sizeof(request) || request.reserved0 != 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (!created_.load(std::memory_order_acquire) || request.surface != handle_) {
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    if (request.buffer_index >= buffer_count_ || request.src_width == 0U || request.src_height == 0U ||
        (request.pitch % kBytesPerRgb565) != 0U ||
        static_cast<uint64_t>(request.pitch) < static_cast<uint64_t>(request.src_width) * kBytesPerRgb565 ||
        (request.flags & ~MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST) != 0U) {
        return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    // A buffer belongs to the Host until RELEASED names it (ABI); presenting
    // it again would tear the frame the display still reads.
    if ((in_flight_mask_.load(std::memory_order_acquire) & (1U << request.buffer_index)) != 0U) {
        return FailService<void>(MICROPIXEL_STATUS_STALE_STATE);
    }
    uint8_t* pixels = nullptr;
    uint32_t length = request.length;
    if (host_buffers_) {
        // The buffer is ours; the request may only name it, whole.
        if (request.pixels != 0U || request.length != 0U || request.pitch != host_pitch_ ||
            request.src_width != host_width_ || request.src_height != host_height_) {
            return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        pixels = host_pixels_[request.buffer_index];
        length = host_pitch_ * host_height_;
    } else {
        if (request.length == 0U || (request.pixels % MICROPIXEL_SURFACE_BUFFER_ALIGNMENT) != 0U ||
            static_cast<uint64_t>(request.pitch) * request.src_height > request.length) {
            return FailService<void>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        if (memory_.resolve == nullptr || !memory_.resolve(memory_.context, request.pixels, request.length, &pixels) ||
            pixels == nullptr) {
            return FailService<void>(MICROPIXEL_STATUS_INVALID_MEMORY);
        }
    }
    const device::DirectSurfacePresentation presentation{
        .pixels = pixels,
        .length = length,
        .pitch = request.pitch,
        .src_width = request.src_width,
        .src_height = request.src_height,
        .flags = request.flags,
        .buffer_index = static_cast<uint8_t>(request.buffer_index),
        // Both buffer kinds are presented in panel order (ABI contract).
        .byte_swapped = native_byte_swapped_,
    };
    // Set the bit before the device can release it: the release clears the
    // bit and only then posts SURFACE_RELEASED, so once the Guest sees the
    // event HostBuffer() already hands the buffer out again.
    in_flight_mask_.fetch_or(1U << request.buffer_index, std::memory_order_release);
    auto result = graphics_.PresentDirectSurface(presentation);
    if (!result) {
        in_flight_mask_.fetch_and(~(1U << request.buffer_index), std::memory_order_release);
        return FailService<void>(result.error().status);
    }
    return {};
}

ServiceResult<void> DirectSurfaceService::Destroy(micropixel_surface_handle_t surface) {
    if (!created_.load(std::memory_order_acquire) || surface != handle_) {
        return FailService<void>(MICROPIXEL_STATUS_NOT_FOUND);
    }
    // The device returns every buffer before DestroyDirectSurface returns;
    // only then retire the handle. The Guest treats DESTROY itself as the
    // release of every buffer, so no RELEASED events are posted meanwhile.
    retiring_.store(true, std::memory_order_release);
    auto result = graphics_.DestroyDirectSurface();
    created_.store(false, std::memory_order_release);
    in_flight_mask_.store(0U, std::memory_order_release);
    handle_ = 0U;
    buffer_count_ = 0U;
    FreeHostBuffers();
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

// Always forwarded: the device may be scanning the composited App Surface out
// through the same presenter without any Direct Surface existing, and that
// path has to hand the panel back to the Host UI on pause as well.
void DirectSurfaceService::Suspend() { graphics_.SuspendDirectSurface(); }

void DirectSurfaceService::Resume() { graphics_.ResumeDirectSurface(); }

void DirectSurfaceService::Shutdown() {
    if (created_.load(std::memory_order_acquire)) {
        retiring_.store(true, std::memory_order_release);
        (void)graphics_.DestroyDirectSurface();
        created_.store(false, std::memory_order_release);
        in_flight_mask_.store(0U, std::memory_order_release);
        handle_ = 0U;
        buffer_count_ = 0U;
        FreeHostBuffers();
    }
}

void DirectSurfaceService::OnBufferReleased(void* context, uint8_t buffer_index, uint64_t timestamp_us) {
    auto* service = static_cast<DirectSurfaceService*>(context);
    if (service == nullptr || !service->created_.load(std::memory_order_acquire)) {
        return;
    }
    if (buffer_index < MICROPIXEL_SURFACE_MAX_BUFFERS) {
        service->in_flight_mask_.fetch_and(~(1U << buffer_index), std::memory_order_release);
    }
    if (service->retiring_.load(std::memory_order_acquire)) {
        return;
    }
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED;
    event.service_id = MICROPIXEL_SERVICE_GRAPHICS;
    event.status = MICROPIXEL_STATUS_OK;
    event.source = service->handle_;
    const auto origin = static_cast<uint64_t>(service->clock_origin_us_);
    event.timestamp_us = timestamp_us >= origin ? timestamp_us - origin : 0U;
    event.sequence = service->sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    micropixel_surface_event_payload_t payload{};
    payload.surface = service->handle_;
    payload.buffer_index = buffer_index;
    payload.timestamp_us = event.timestamp_us;
    static_assert(sizeof(payload) <= sizeof(event.payload));
    std::memcpy(event.payload, &payload, sizeof(payload));
    if (!service->events_.PushRequired(event)) {
        ESP_LOGW(kTag, "SURFACE_RELEASED dropped: buffer=%u", static_cast<unsigned>(buffer_index));
    }
}

}  // namespace micropixel::runtime
