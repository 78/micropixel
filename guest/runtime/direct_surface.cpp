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
using micropixel::runtime::LoadGraphicsLimits;
using micropixel::runtime::LoadPhysicalGraphicsInfo;
using micropixel::runtime::ZeroBytes;

namespace {

// The Host allows one Direct Surface per App, so buffer ownership lives here
// rather than in the move-only surface object: Application updates the mask
// when it decodes SURFACE_RELEASED, DirectSurface reads it.
struct DirectSurfaceState final {
    uint32_t handle{};
    uint32_t busy_mask{};
};

DirectSurfaceState direct_surface_state{};

// Buffer geometry shared by both surface kinds, derived from the panel size.
struct BufferGeometry final {
    uint32_t width{};
    uint32_t height{};
    // Bytes from one buffer start to the next; every buffer starts on a
    // MICROPIXEL_SURFACE_BUFFER_ALIGNMENT boundary so the Host can hand it to
    // DMA without a staging copy.
    uint32_t stride{};
};

[[nodiscard]] BufferGeometry GeometryFor(const micropixel_graphics_info_t& raw, uint32_t upscale) {
    BufferGeometry geometry{};
    geometry.width = raw.width / upscale;
    geometry.height = raw.height / upscale;
    geometry.stride = AlignUp(geometry.width * 2U * geometry.height, MICROPIXEL_SURFACE_BUFFER_ALIGNMENT);
    return geometry;
}

void FreeStorage(uint8_t* storage) {
    if (storage != nullptr) {
        ::operator delete(storage, std::align_val_t{MICROPIXEL_SURFACE_BUFFER_ALIGNMENT});
    }
}

}  // namespace

namespace micropixel {

struct DirectSurfaceCreation final {
    uint32_t handle{};
    uint32_t width{};
    uint32_t height{};
    uint32_t buffer_width{};
    uint32_t buffer_height{};
    uint32_t buffer_count{};
    uint32_t native_flags{};
    uint16_t max_full_frame_fps{};
};

namespace {

// Validates the request and asks the Host for a surface. `guest_buffers`
// selects who owns the pixels; the caller has already allocated them when true.
[[nodiscard]] Result<DirectSurfaceCreation> CreateSurfaceHandle(uint32_t buffer_count, uint32_t upscale,
                                                                bool guest_buffers) {
    const micropixel_graphics_info_t& raw = LoadPhysicalGraphicsInfo();
    if (buffer_count == 0U || buffer_count > LoadGraphicsLimits().max_surface_buffers || upscale == 0U ||
        raw.width % upscale != 0U || raw.height % upscale != 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    if (raw.size < sizeof(raw) || direct_surface_state.handle != 0U) {
        // Graphics < 1.5 Host, or a surface already exists on this side.
        return unexpected(ErrorFromStatus(direct_surface_state.handle != 0U ? MICROPIXEL_STATUS_RESOURCE_EXHAUSTED
                                                                            : MICROPIXEL_STATUS_UNSUPPORTED));
    }
    const BufferGeometry geometry = GeometryFor(raw, upscale);

    micropixel_surface_create_request_t request{};
    request.size = sizeof(request);
    // Buffer size, not panel size: Host buffers are allocated at it and every
    // present enlarges by `upscale`.
    request.width = geometry.width;
    request.height = geometry.height;
    request.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    request.buffer_count = buffer_count;
    request.flags = guest_buffers ? MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS : 0U;
    micropixel_surface_create_response_t response{};
    uint32_t response_size = 0U;
    const int32_t status = CallService(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_SURFACE_CREATE, &request,
                                       sizeof(request), &response, sizeof(response), response_size);
    if (status != MICROPIXEL_STATUS_OK) {
        return unexpected(ErrorFromStatus(status));
    }
    if (response_size < sizeof(response) || response.size < sizeof(response) || response.surface_handle == 0U ||
        response.native_pixel_format != MICROPIXEL_PIXEL_FORMAT_RGB565) {
        runtime::Panic("surface.create.response", MICROPIXEL_STATUS_INTERNAL);
    }
    direct_surface_state.handle = response.surface_handle;
    direct_surface_state.busy_mask = 0U;
    DirectSurfaceCreation creation{};
    creation.handle = response.surface_handle;
    creation.width = raw.width;
    creation.height = raw.height;
    creation.buffer_width = geometry.width;
    creation.buffer_height = geometry.height;
    creation.buffer_count = buffer_count;
    creation.native_flags = response.native_flags;
    creation.max_full_frame_fps = response.max_full_frame_fps;
    return creation;
}

}  // namespace

// ---- Renderer factories ----------------------------------------------------

Result<HostSurface> Renderer::CreateHostSurface(uint32_t buffer_count, uint32_t upscale) const {
    auto creation = CreateSurfaceHandle(buffer_count, upscale, false);
    if (!creation.has_value()) {
        return unexpected(creation.error());
    }
    return HostSurface{creation.value()};
}

Result<GuestSurface> Renderer::CreateGuestSurface(uint32_t buffer_count, uint32_t upscale) const {
    const micropixel_graphics_info_t& raw = LoadPhysicalGraphicsInfo();
    if (buffer_count == 0U || buffer_count > LoadGraphicsLimits().max_surface_buffers || upscale == 0U ||
        raw.width % upscale != 0U || raw.height % upscale != 0U) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_INVALID_ARGUMENT));
    }
    // The Host validates the presented pixel range against Guest memory, so
    // the buffers exist before the surface does.
    const BufferGeometry geometry = GeometryFor(raw, upscale);
    auto* storage =
        static_cast<uint8_t*>(::operator new(static_cast<size_t>(geometry.stride) * buffer_count,
                                             std::align_val_t{MICROPIXEL_SURFACE_BUFFER_ALIGNMENT}, std::nothrow));
    if (storage == nullptr) {
        return unexpected(ErrorFromStatus(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED));
    }
    ZeroBytes(storage, geometry.stride * buffer_count);
    auto creation = CreateSurfaceHandle(buffer_count, upscale, true);
    if (!creation.has_value()) {
        FreeStorage(storage);
        return unexpected(creation.error());
    }
    return GuestSurface{creation.value(), storage};
}

// ---- DirectSurface: handle, geometry and frame pacing ----------------------

DirectSurface::DirectSurface(const DirectSurfaceCreation& creation)
    : handle_(creation.handle),
      width_(creation.width),
      height_(creation.height),
      buffer_width_(creation.buffer_width),
      buffer_height_(creation.buffer_height),
      buffer_count_(creation.buffer_count),
      max_full_frame_fps_(creation.max_full_frame_fps),
      rgb565_byte_swapped_((creation.native_flags & MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED) != 0U),
      direct_scanout_((creation.native_flags & MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT) != 0U) {}

DirectSurface::DirectSurface(DirectSurface&& other) noexcept
    : handle_(other.handle_),
      width_(other.width_),
      height_(other.height_),
      buffer_width_(other.buffer_width_),
      buffer_height_(other.buffer_height_),
      buffer_count_(other.buffer_count_),
      guest_pixels_base_(other.guest_pixels_base_),
      guest_pixels_stride_(other.guest_pixels_stride_),
      guest_pixels_length_(other.guest_pixels_length_),
      max_full_frame_fps_(other.max_full_frame_fps_),
      rgb565_byte_swapped_(other.rgb565_byte_swapped_),
      direct_scanout_(other.direct_scanout_) {
    other.handle_ = 0U;
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
        guest_pixels_base_ = other.guest_pixels_base_;
        guest_pixels_stride_ = other.guest_pixels_stride_;
        guest_pixels_length_ = other.guest_pixels_length_;
        max_full_frame_fps_ = other.max_full_frame_fps_;
        rgb565_byte_swapped_ = other.rgb565_byte_swapped_;
        direct_scanout_ = other.direct_scanout_;
        other.handle_ = 0U;
    }
    return *this;
}

DirectSurface::~DirectSurface() { Reset(); }

void DirectSurface::Reset() {
    if (handle_ != 0U) {
        // DESTROY returns every buffer before it completes, so Guest storage
        // is safe to free as soon as the call comes back.
        micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
        (void)CallVoid(GraphicsService(), MICROPIXEL_GRAPHICS_METHOD_SURFACE_DESTROY, &request, sizeof(request));
        if (direct_surface_state.handle == handle_) {
            direct_surface_state.handle = 0U;
            direct_surface_state.busy_mask = 0U;
        }
        handle_ = 0U;
    }
    width_ = 0U;
    height_ = 0U;
    buffer_width_ = 0U;
    buffer_height_ = 0U;
    buffer_count_ = 0U;
    guest_pixels_base_ = 0U;
    guest_pixels_stride_ = 0U;
    guest_pixels_length_ = 0U;
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
    request.surface_handle = handle_;
    request.buffer_index = index;
    // Host buffers are named by index alone (pixels/length stay 0).
    if (guest_pixels_base_ != 0U) {
        request.pixels = guest_pixels_base_ + guest_pixels_stride_ * index;
        request.length = guest_pixels_length_;
    }
    request.pitch = buffer_width_ * 2U;
    request.source_width = buffer_width_;
    request.source_height = buffer_height_;
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

// ---- GuestSurface: App-written buffers -------------------------------------

GuestSurface::GuestSurface(const DirectSurfaceCreation& creation, uint8_t* storage)
    : DirectSurface(creation), storage_(storage) {
    SetGuestPixels(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(storage_)), buffer_stride(), buffer_bytes());
}

GuestSurface::GuestSurface(GuestSurface&& other) noexcept
    : DirectSurface(static_cast<DirectSurface&&>(other)), storage_(other.storage_) {
    other.storage_ = nullptr;
}

GuestSurface& GuestSurface::operator=(GuestSurface&& other) noexcept {
    if (this != &other) {
        Reset();
        DirectSurface::operator=(static_cast<DirectSurface&&>(other));
        storage_ = other.storage_;
        other.storage_ = nullptr;
    }
    return *this;
}

GuestSurface::~GuestSurface() { Reset(); }

void GuestSurface::Reset() {
    // Destroy first: the Host may still point into the storage until then.
    DirectSurface::Reset();
    FreeStorage(storage_);
    storage_ = nullptr;
}

uint32_t GuestSurface::buffer_stride() const { return AlignUp(buffer_bytes(), MICROPIXEL_SURFACE_BUFFER_ALIGNMENT); }

uint16_t* GuestSurface::Buffer(uint32_t index) {
    if (!valid() || storage_ == nullptr || index >= buffer_count()) {
        return nullptr;
    }
    return reinterpret_cast<uint16_t*>(storage_ + static_cast<size_t>(buffer_stride()) * index);
}

const uint16_t* GuestSurface::Buffer(uint32_t index) const {
    if (!valid() || storage_ == nullptr || index >= buffer_count()) {
        return nullptr;
    }
    return reinterpret_cast<const uint16_t*>(storage_ + static_cast<size_t>(buffer_stride()) * index);
}

}  // namespace micropixel

namespace micropixel::runtime {

void ReleaseSurfaceBuffer(uint32_t handle, uint32_t buffer_index) {
    if (handle == direct_surface_state.handle) {
        direct_surface_state.busy_mask &= ~(1U << buffer_index);
    }
}

}  // namespace micropixel::runtime
