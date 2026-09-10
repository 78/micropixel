// DirectSurfaceService: ABI validation in front of the Graphics device, buffer
// release -> SURFACE_RELEASED translation and the pause/teardown protocol.
#include <cstdlib>
#include <cstring>

#include "device/contracts/graphics.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/services/direct_surface_service.hpp"

namespace {

constexpr uint32_t kWidth = 480U;
constexpr uint32_t kHeight = 480U;
constexpr uint32_t kFrameBytes = kWidth * kHeight * 2U;
// Guest linear memory model: offsets below kGuestMemoryBytes resolve, everything
// else is rejected exactly like wasm_runtime_validate_app_addr would.
constexpr uint32_t kGuestMemoryBytes = 4U * 1024U * 1024U;

void Require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

class FakeGraphics final : public micropixel::device::Graphics {
   public:
    [[nodiscard]] bool Available() const override { return true; }
    [[nodiscard]] int32_t GetInfo(micropixel_graphics_info_t&) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t Submit(const uint8_t*, uint32_t, const micropixel::device::TextureAccess&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t LoadFont(const micropixel::device::FontResourceView&, micropixel_font_info_t&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t ReleaseFont(micropixel_font_handle_t) override { return MICROPIXEL_STATUS_UNSUPPORTED; }
    [[nodiscard]] int32_t MeasureText(micropixel_font_handle_t, const char*, uint32_t,
                                      micropixel_text_metrics_t&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t DrawText(const micropixel::device::TextTarget&, int32_t, int32_t, uint32_t,
                                   micropixel_font_handle_t, const char*, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t CopyOpaqueBlocks(const micropixel::device::PixelTarget&,
                                           const micropixel::device::OpaqueCopyBlock*, uint32_t) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t ScaleBitmap(const micropixel::device::BitmapView&,
                                      const micropixel::device::BitmapView&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    [[nodiscard]] int32_t ShowLaunchBitmap(const micropixel::device::BitmapView&) override {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    void DismissLaunchBitmap() override {}
    void ReleaseGuestResources() override { (void)DestroyDirectSurface(); }

    [[nodiscard]] int32_t CreateDirectSurface(const micropixel::device::DirectSurfaceConfig& config,
                                              const micropixel::device::DirectSurfaceReleaseSink& sink,
                                              micropixel::device::DirectSurfaceInfo& info_out) override {
        if (created) {
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        if (config.width != kWidth || config.height != kHeight) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }
        created = true;
        this->config = config;
        this->sink = sink;
        in_flight_mask = 0U;
        info_out = {
            .native_pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565,
            .native_flags = MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED | MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT,
            .max_full_frame_fps = 42U};
        return MICROPIXEL_STATUS_OK;
    }

    [[nodiscard]] int32_t PresentDirectSurface(
        const micropixel::device::DirectSurfacePresentation& presentation) override {
        if (!created) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        const uint8_t bit = static_cast<uint8_t>(1U << presentation.buffer_index);
        if ((in_flight_mask & bit) != 0U) {
            return MICROPIXEL_STATUS_STALE_STATE;
        }
        in_flight_mask |= bit;
        last = presentation;
        ++present_count;
        return MICROPIXEL_STATUS_OK;
    }

    void SuspendDirectSurface() override {
        ++suspend_count;
        ReleaseAll();
    }

    void ResumeDirectSurface() override { ++resume_count; }

    [[nodiscard]] int32_t DestroyDirectSurface() override {
        if (!created) {
            return MICROPIXEL_STATUS_NOT_FOUND;
        }
        ReleaseAll();
        created = false;
        ++destroy_count;
        return MICROPIXEL_STATUS_OK;
    }

    // Test control: the panel finished with one buffer.
    void Release(uint8_t buffer_index, uint64_t timestamp_us) {
        in_flight_mask &= static_cast<uint8_t>(~(1U << buffer_index));
        sink.release(sink.context, buffer_index, timestamp_us);
    }

    bool created{};
    micropixel::device::DirectSurfaceConfig config{};
    micropixel::device::DirectSurfaceReleaseSink sink{};
    micropixel::device::DirectSurfacePresentation last{};
    uint8_t in_flight_mask{};
    uint32_t present_count{};
    uint32_t suspend_count{};
    uint32_t resume_count{};
    uint32_t destroy_count{};

   private:
    void ReleaseAll() {
        for (uint8_t index = 0U; index < micropixel::device::graphics_limits::kMaxSurfaceBuffers; ++index) {
            if ((in_flight_mask & (1U << index)) != 0U) {
                Release(index, 5000U);
            }
        }
    }
};

alignas(64) uint8_t g_guest_memory[64U]{};

bool ResolveGuestMemory(void*, uint32_t offset, uint32_t length, uint8_t** host_out) {
    if (length == 0U || offset > kGuestMemoryBytes || length > kGuestMemoryBytes - offset) {
        return false;
    }
    // Any in-range offset maps to the same Host buffer; the service only
    // forwards the pointer, it never dereferences it.
    *host_out = g_guest_memory;
    return true;
}

// GUEST_BUFFERS surface: the Guest owns and addresses the buffers.
micropixel_surface_create_request_t CreateRequest(uint32_t buffer_count) {
    micropixel_surface_create_request_t request{};
    request.size = sizeof(request);
    request.width = kWidth;
    request.height = kHeight;
    request.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    request.buffer_count = buffer_count;
    request.flags = MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS;
    return request;
}

// Default surface: Host-owned buffers named by index.
micropixel_surface_create_request_t HostCreateRequest(uint32_t buffer_count) {
    micropixel_surface_create_request_t request = CreateRequest(buffer_count);
    request.flags = 0U;
    return request;
}

micropixel_surface_present_request_t PresentRequest(micropixel_surface_handle_t surface_handle, uint32_t buffer_index) {
    micropixel_surface_present_request_t request{};
    request.size = sizeof(request);
    request.surface_handle = surface_handle;
    request.buffer_index = buffer_index;
    request.pixels = 0x10000U + buffer_index * kFrameBytes;
    request.length = kFrameBytes;
    request.pitch = kWidth * 2U;
    request.source_width = kWidth;
    request.source_height = kHeight;
    return request;
}

micropixel_surface_present_request_t HostPresentRequest(micropixel_surface_handle_t surface_handle,
                                                        uint32_t buffer_index) {
    micropixel_surface_present_request_t request = PresentRequest(surface_handle, buffer_index);
    request.pixels = 0U;
    request.length = 0U;
    return request;
}

bool PopRelease(micropixel::runtime::EventQueue& events, micropixel_surface_handle_t surface_handle,
                uint32_t buffer_index, uint64_t expected_timestamp_us) {
    micropixel_event_t event{};
    if (events.Wait(event, 0U) != micropixel::runtime::EventWaitResult::kReceived) {
        return false;
    }
    micropixel_surface_event_payload_t payload{};
    std::memcpy(&payload, event.payload, sizeof(payload));
    return event.service_id == MICROPIXEL_SERVICE_GRAPHICS &&
           event.event_id == MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED && event.source == surface_handle &&
           event.status == MICROPIXEL_STATUS_OK && event.timestamp_us == expected_timestamp_us &&
           payload.surface_handle == surface_handle && payload.buffer_index == buffer_index &&
           payload.timestamp_us == expected_timestamp_us;
}

bool QueueEmpty(micropixel::runtime::EventQueue& events) {
    micropixel_event_t event{};
    return events.Wait(event, 0U) == micropixel::runtime::EventWaitResult::kTimeout;
}

}  // namespace

int main() {
    using micropixel::runtime::DirectSurfaceService;
    using micropixel::runtime::EventQueue;

    FakeGraphics backend;
    micropixel::device::GraphicsService graphics{backend, micropixel::device::DisplayInfo{}};
    EventQueue events;
    Require(events.valid());
    constexpr int64_t kClockOrigin = 1000U;
    DirectSurfaceService service{graphics, events, kClockOrigin};

    // Without bound Guest memory no GUEST_BUFFERS surface can be created.
    Require(!service.Create(CreateRequest(2U)) &&
            service.Create(CreateRequest(2U)).error().status == MICROPIXEL_STATUS_UNSUPPORTED);
    // A Guest whose linear memory may relocate is refused as well: the Host
    // would otherwise keep pointers memory.grow can free.
    service.BindGuestMemory({.context = nullptr, .resolve = ResolveGuestMemory, .stable_base = false});
    Require(service.Create(CreateRequest(2U)).error().status == MICROPIXEL_STATUS_UNSUPPORTED);
    Require(!backend.created);
    // Host buffers need neither: the Guest never addresses them.
    {
        micropixel::runtime::HostBufferView none{};
        Require(service.HostBuffer(0U, 0U, none) == MICROPIXEL_STATUS_NOT_FOUND);
        auto host = service.Create(HostCreateRequest(2U));
        Require(host.has_value() && backend.created && backend.config.flags == 0U);
        Require(service.native_byte_swapped());
        micropixel::runtime::HostBufferView view{};
        Require(service.HostBuffer(host->surface_handle, 0U, view) == MICROPIXEL_STATUS_OK && view.pixels != nullptr &&
                view.width == kWidth && view.height == kHeight && view.pitch == kWidth * 2U);
        Require(view.pixels[0] == 0U && view.pixels[kFrameBytes - 1U] == 0U);
        Require(service.HostBuffer(host->surface_handle, 2U, view) == MICROPIXEL_STATUS_NOT_FOUND);
        micropixel::runtime::HostBufferView other{};
        Require(service.HostBuffer(host->surface_handle, 1U, other) == MICROPIXEL_STATUS_OK &&
                other.pixels != view.pixels);
        // A Host-buffer present names the buffer only; addresses are refused,
        // and so is a geometry other than the buffer's.
        auto present = PresentRequest(host->surface_handle, 0U);
        Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
        present = HostPresentRequest(host->surface_handle, 0U);
        present.source_width = kWidth / 2U;
        present.pitch = kWidth;
        present.flags = MICROPIXEL_SURFACE_PRESENT_SCALE_NEAREST;
        Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
        Require(service.Present(HostPresentRequest(host->surface_handle, 0U)).has_value());
        Require(backend.last.pixels == view.pixels && backend.last.length == kFrameBytes &&
                backend.last.pitch == kWidth * 2U && backend.last.byte_swapped && backend.last.flags == 0U);
        // While in flight the kernels may not write it and it cannot be
        // presented again; the release hands it back.
        Require(service.HostBuffer(host->surface_handle, 0U, view) == MICROPIXEL_STATUS_STALE_STATE);
        Require(service.Present(HostPresentRequest(host->surface_handle, 0U)).error().status ==
                MICROPIXEL_STATUS_STALE_STATE);
        Require(service.HostBuffer(host->surface_handle, 1U, other) == MICROPIXEL_STATUS_OK);
        backend.Release(0U, 2000U);
        Require(PopRelease(events, host->surface_handle, 0U, 1000U));
        Require(service.HostBuffer(host->surface_handle, 0U, view) == MICROPIXEL_STATUS_OK);
        Require(service.Destroy(host->surface_handle).has_value());
        Require(service.HostBuffer(host->surface_handle, 0U, view) == MICROPIXEL_STATUS_NOT_FOUND);
        Require(QueueEmpty(events));
    }

    service.BindGuestMemory({.context = nullptr, .resolve = ResolveGuestMemory, .stable_base = true});

    // Create validation happens before the device is touched.
    auto bad = CreateRequest(0U);
    Require(service.Create(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = CreateRequest(micropixel::device::graphics_limits::kMaxSurfaceBuffers + 1U);
    Require(service.Create(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = CreateRequest(2U);
    bad.pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888;
    Require(service.Create(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = CreateRequest(2U);
    bad.flags = 2U;
    Require(service.Create(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = CreateRequest(2U);
    bad.size = sizeof(bad) - 1U;
    Require(service.Create(bad).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    Require(!backend.created);

    // Device rejection leaves the service without a surface.
    auto wrong_size = CreateRequest(2U);
    wrong_size.width = kWidth / 2U;
    Require(service.Create(wrong_size).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    Require(!backend.created);
    Require(service.Present(PresentRequest(1U, 0U)).error().status == MICROPIXEL_STATUS_NOT_FOUND);

    auto created = service.Create(CreateRequest(2U));
    Require(created.has_value());
    Require(created->size == sizeof(*created) && created->surface_handle != 0U);
    Require(created->native_pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB565 &&
            created->native_flags ==
                (MICROPIXEL_SURFACE_NATIVE_RGB565_BYTE_SWAPPED | MICROPIXEL_SURFACE_NATIVE_DIRECT_SCANOUT) &&
            created->max_full_frame_fps == 42U);
    Require(backend.created && backend.config.buffer_count == 2U && backend.config.flags == 0U);
    const micropixel_surface_handle_t surface = created->surface_handle;
    Require(service.Create(CreateRequest(1U)).error().status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);

    // Present validation: every malformed request is refused before the device
    // sees it, and the Guest range is checked through the bound resolver.
    auto present = PresentRequest(surface + 1U, 0U);
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_NOT_FOUND);
    present = PresentRequest(surface, 2U);
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    present = PresentRequest(surface, 0U);
    present.pixels += 2U;
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    present = PresentRequest(surface, 0U);
    present.length = kFrameBytes - 2U;
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    present = PresentRequest(surface, 0U);
    present.pitch = kWidth * 2U - 2U;
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    present = PresentRequest(surface, 0U);
    present.pitch += 1U;
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    present = PresentRequest(surface, 0U);
    present.flags = 0x4U;
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    present = PresentRequest(surface, 0U);
    present.pixels = kGuestMemoryBytes - 64U;  // aligned but runs off the end
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);
    present = PresentRequest(surface, 0U);
    present.pixels = 0xffff0000U;
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_INVALID_MEMORY);
    Require(backend.present_count == 1U);  // only the Host-buffer present above

    // Valid presents reach the device with the resolved pointer, the panel's
    // byte order (the ABI makes the Guest write it) and the flags.
    present = PresentRequest(surface, 0U);
    Require(service.Present(present).has_value());
    Require(backend.present_count == 2U && backend.last.pixels == g_guest_memory && backend.last.buffer_index == 0U &&
            backend.last.pitch == kWidth * 2U && backend.last.length == kFrameBytes && backend.last.byte_swapped &&
            backend.last.flags == 0U);
    // The same buffer cannot be presented again while the device holds it.
    Require(service.Present(present).error().status == MICROPIXEL_STATUS_STALE_STATE);
    Require(service.Present(PresentRequest(surface, 1U)).has_value());
    Require(backend.in_flight_mask == 0x3U);

    // Releases become SURFACE_RELEASED events with session-relative timestamps.
    Require(QueueEmpty(events));
    backend.Release(0U, 1500U);
    Require(PopRelease(events, surface, 0U, 500U));
    // A timestamp before the session clock origin clamps to zero.
    backend.Release(1U, 200U);
    Require(PopRelease(events, surface, 1U, 0U));
    Require(QueueEmpty(events));
    Require(service.Present(PresentRequest(surface, 0U)).has_value());

    // Suspend returns every in-flight buffer without destroying the surface.
    service.Suspend();
    Require(backend.suspend_count == 1U && backend.created && backend.in_flight_mask == 0U);
    service.Resume();
    Require(backend.resume_count == 1U);
    Require(PopRelease(events, surface, 0U, 4000U));
    Require(QueueEmpty(events));
    Require(service.Present(PresentRequest(surface, 1U)).has_value());

    // Destroy drains the device first, then retires the handle. The releases
    // the device performs meanwhile are not posted: DESTROY itself returns
    // every buffer to the Guest, and the presenter task must never block on a
    // Guest queue that a departing Guest no longer drains.
    Require(service.Destroy(surface + 1U).error().status == MICROPIXEL_STATUS_NOT_FOUND);
    Require(service.Destroy(surface).has_value());
    Require(!backend.created && backend.destroy_count == 2U);  // Host-buffer surface counted one
    Require(backend.in_flight_mask == 0U);
    Require(QueueEmpty(events));
    Require(service.Present(PresentRequest(surface, 0U)).error().status == MICROPIXEL_STATUS_NOT_FOUND);
    Require(service.Destroy(surface).error().status == MICROPIXEL_STATUS_NOT_FOUND);

    // A new surface gets a fresh handle; Shutdown tears it down for teardown.
    auto second = service.Create(CreateRequest(3U));
    Require(second.has_value() && second->surface_handle != surface);
    Require(service.Present(PresentRequest(second->surface_handle, 2U)).has_value());
    service.Shutdown();
    Require(!backend.created && backend.destroy_count == 3U);
    Require(QueueEmpty(events));
    // Suspend/Resume still reach the device without a surface (App Surface
    // scanout shares the presenter); Shutdown is a no-op.
    service.Suspend();
    service.Resume();
    service.Shutdown();
    Require(backend.suspend_count == 2U && backend.resume_count == 2U && backend.destroy_count == 3U);
    return 0;
}
