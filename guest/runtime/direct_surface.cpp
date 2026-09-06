#include <new>

#include "runtime/direct_surface_state.hpp"
#include "runtime/display_context.hpp"
#include "runtime/service_binding.hpp"
#include "sdk/graphics.hpp"

using micropixel::runtime::AlignUp;
using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::GraphicsService;
using micropixel::runtime::LoadPhysicalGraphicsInfo;
using micropixel::runtime::ZeroBytes;

namespace {

// The Host allows one Direct Surface per App, so buffer ownership lives here
// rather than in the move-only DirectSurface object: Application updates the
// mask when it decodes SURFACE_RELEASED, DirectSurface reads it.
struct DirectSurfaceState final {
    uint32_t handle{};
    uint32_t busy_mask{};
};

DirectSurfaceState direct_surface_state{};

}  // namespace

namespace micropixel {

Result<DirectSurface> Renderer::CreateDirectSurface(uint32_t buffer_count, DirectSurfaceBuffers buffers,
                                                    uint32_t upscale) const {
    const micropixel_graphics_info_t& raw = LoadPhysicalGraphicsInfo();
    const bool guest_buffers = buffers == DirectSurfaceBuffers::kGuest;
    if (buffer_count == 0U || buffer_count > MICROPIXEL_SURFACE_MAX_BUFFERS || upscale == 0U ||
        raw.width % upscale != 0U || raw.height % upscale != 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    if (raw.size < sizeof(raw) || direct_surface_state.handle != 0U) {
        // Graphics < 1.5 Host, or a surface already exists on this side.
        return unexpected(ErrorFromStatus(direct_surface_state.handle != 0U ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED
                                                                            : MICROPIXEL_STATUS_UNSUPPORTED));
    }
    const uint32_t buffer_width = raw.width / upscale;
    const uint32_t buffer_height = raw.height / upscale;
    const uint32_t pitch = buffer_width * 2U;
    // Every buffer starts on a MICROPIXEL_SURFACE_BUFFER_ALIGNMENT boundary so
    // the Host can hand it to DMA without a staging copy.
    const uint32_t buffer_bytes = AlignUp(pitch * buffer_height, MICROPIXEL_SURFACE_BUFFER_ALIGNMENT);
    uint8_t* storage = nullptr;
    if (guest_buffers) {
        storage =
            static_cast<uint8_t*>(::operator new(static_cast<size_t>(buffer_bytes) * buffer_count,
                                                 std::align_val_t{MICROPIXEL_SURFACE_BUFFER_ALIGNMENT}, std::nothrow));
        if (storage == nullptr) {
            return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED));
        }
        ZeroBytes(storage, buffer_bytes * buffer_count);
    }

    micropixel_surface_create_request_t request{};
    request.size = sizeof(request);
    // Buffer size, not panel size: Host buffers are allocated at it and every
    // present enlarges by `upscale`.
    request.width = buffer_width;
    request.height = buffer_height;
    request.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    request.buffer_count = buffer_count;
    request.flags = guest_buffers ? MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS : 0U;
    micropixel_surface_create_response_t response{};
    uint32_t response_size = 0U;
    const int32_t status = CallService(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &request,
                                       sizeof(request), &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        if (storage != nullptr) {
            ::operator delete(storage, std::align_val_t{MICROPIXEL_SURFACE_BUFFER_ALIGNMENT});
        }
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.surface == 0U ||
        response.native_pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) {
        runtime::Panic("surface.create.response", MICROPIXEL_STATUS_INTERNAL);
    }
    direct_surface_state.handle = response.surface;
    direct_surface_state.busy_mask = 0U;
    return DirectSurface{response.surface,
                         raw.width,
                         raw.height,
                         buffer_width,
                         buffer_height,
                         buffer_count,
                         storage,
                         response.native_flags,
                         response.max_full_frame_fps};
}

DirectSurface::DirectSurface(uint32_t handle, uint32_t width, uint32_t height, uint32_t buffer_width,
                             uint32_t buffer_height, uint32_t buffer_count, uint8_t* storage, uint32_t native_flags,
                             uint16_t max_full_frame_fps)
    : handle_(handle),
      width_(width),
      height_(height),
      buffer_width_(buffer_width),
      buffer_height_(buffer_height),
      buffer_count_(buffer_count),
      storage_(storage),
      max_full_frame_fps_(max_full_frame_fps),
      rgb565_byte_swapped_((native_flags & MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED) != 0U),
      direct_scanout_((native_flags & MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT) != 0U) {}

DirectSurface::DirectSurface(DirectSurface&& other) noexcept
    : handle_(other.handle_),
      width_(other.width_),
      height_(other.height_),
      buffer_width_(other.buffer_width_),
      buffer_height_(other.buffer_height_),
      buffer_count_(other.buffer_count_),
      storage_(other.storage_),
      max_full_frame_fps_(other.max_full_frame_fps_),
      rgb565_byte_swapped_(other.rgb565_byte_swapped_),
      direct_scanout_(other.direct_scanout_) {
    other.handle_ = 0U;
    other.storage_ = nullptr;
}

DirectSurface& DirectSurface::operator=(DirectSurface&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        width_ = other.width_;
        height_ = other.height_;
        buffer_width_ = other.buffer_width_;
        buffer_height_ = other.buffer_height_;
        buffer_count_ = other.buffer_count_;
        storage_ = other.storage_;
        max_full_frame_fps_ = other.max_full_frame_fps_;
        rgb565_byte_swapped_ = other.rgb565_byte_swapped_;
        direct_scanout_ = other.direct_scanout_;
        other.handle_ = 0U;
        other.storage_ = nullptr;
    }
    return *this;
}

DirectSurface::~DirectSurface() { Reset(); }

void DirectSurface::Reset() {
    if (handle_ != 0U) {
        // DESTROY returns every buffer before it completes, so the storage is
        // safe to free as soon as the call comes back.
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        (void)CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY, &request, sizeof(request));
        if (direct_surface_state.handle == handle_) {
            direct_surface_state.handle = 0U;
            direct_surface_state.busy_mask = 0U;
        }
        handle_ = 0U;
    }
    if (storage_ != nullptr) {
        ::operator delete(storage_, std::align_val_t{MICROPIXEL_SURFACE_BUFFER_ALIGNMENT});
        storage_ = nullptr;
    }
    width_ = 0U;
    height_ = 0U;
    buffer_width_ = 0U;
    buffer_height_ = 0U;
    buffer_count_ = 0U;
}

uint16_t* DirectSurface::Buffer(uint32_t index) {
    if (!valid() || storage_ == nullptr || index >= buffer_count_) {
        return nullptr;
    }
    const uint32_t stride = AlignUp(buffer_bytes(), MICROPIXEL_SURFACE_BUFFER_ALIGNMENT);
    return reinterpret_cast<uint16_t*>(storage_ + static_cast<size_t>(stride) * index);
}

const uint16_t* DirectSurface::Buffer(uint32_t index) const {
    if (!valid() || storage_ == nullptr || index >= buffer_count_) {
        return nullptr;
    }
    const uint32_t stride = AlignUp(buffer_bytes(), MICROPIXEL_SURFACE_BUFFER_ALIGNMENT);
    return reinterpret_cast<const uint16_t*>(storage_ + static_cast<size_t>(stride) * index);
}

bool DirectSurface::Busy(uint32_t index) const {
    return valid() && direct_surface_state.handle == handle_ && index < buffer_count_ &&
           (direct_surface_state.busy_mask & (1U << index)) != 0U;
}

bool DirectSurface::AcquireFree(uint32_t& index_out) const {
    if (!valid()) {
        return false;
    }
    for (uint32_t index = 0U; index < buffer_count_; ++index) {
        if (!Busy(index)) {
            index_out = index;
            return true;
        }
    }
    return false;
}

Result<void> DirectSurface::Present(uint32_t index) {
    if (!valid() || index >= buffer_count_) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    if (Busy(index)) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_STALE_STATE));
    }
    micropixel_surface_present_request_t request{};
    request.size = sizeof(request);
    request.surface = handle_;
    request.buffer_index = index;
    // Host buffers are named by index alone (pixels/length stay 0).
    if (storage_ != nullptr) {
        request.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Buffer(index)));
        request.length = buffer_bytes();
    }
    request.pitch = pitch();
    request.src_width = buffer_width_;
    request.src_height = buffer_height_;
    request.flags =
        buffer_width_ != width_ || buffer_height_ != height_ ? MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST : 0U;
    const int32_t status =
        CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_SURFACE_PRESENT, &request, sizeof(request));
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    direct_surface_state.busy_mask |= 1U << index;
    return {};
}

}  // namespace micropixel

namespace micropixel::runtime {

void ReleaseSurfaceBuffer(uint32_t handle, uint32_t buffer_index) {
    if (handle == direct_surface_state.handle) {
        direct_surface_state.busy_mask &= ~(1U << buffer_index);
    }
}

}  // namespace micropixel::runtime
