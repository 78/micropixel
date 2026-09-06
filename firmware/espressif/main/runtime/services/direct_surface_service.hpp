#ifndef MICROPIXEL_RUNTIME_SERVICES_DIRECT_SURFACE_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_DIRECT_SURFACE_SERVICE_HPP

#include <atomic>
#include <cstdint>

#include "abi/micropixel_abi.h"
#include "device/device_services.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

// Resolves a Guest linear-memory range to a Host pointer. Bound by the session
// once the WAMR instance exists; the Host never trusts a Guest offset without it.
struct GuestMemoryAccess final {
    void* context{};
    bool (*resolve)(void* context, uint32_t offset, uint32_t length, uint8_t** host_out){};
    // The linear-memory base cannot move for the instance lifetime (the
    // Bundle declared PINNED_MEMORY). Without it a resolved pointer is only
    // good until the Guest's next memory.grow, so no GUEST_BUFFERS Direct
    // Surface may be created: its buffers stay in flight across calls.
    bool stable_base{};
};

// One Host-owned Direct Surface buffer handed to raster kernels.
struct HostBufferView final {
    uint8_t* pixels{};
    uint32_t width{};
    uint32_t height{};
    uint32_t pitch{};
};

// Graphics 1.5 Direct Surface: validates SURFACE_* requests against the ABI,
// forwards them to the Graphics device and turns buffer releases into
// MICROPIXEL_GRAPHICS_EVENT_SURFACE_RELEASED events. By default the buffers
// are Host PSRAM frames in the panel's byte order that the Guest only names by
// index (present, raster target); with MICROPIXEL_SURFACE_CREATE_GUEST_BUFFERS
// they are Guest memory the Guest writes itself, which needs a pinned base.
class DirectSurfaceService final {
   public:
    DirectSurfaceService(device::GraphicsService& graphics, EventQueue& events, int64_t clock_origin_us);
    DirectSurfaceService(const DirectSurfaceService&) = delete;
    DirectSurfaceService& operator=(const DirectSurfaceService&) = delete;
    ~DirectSurfaceService();

    void BindGuestMemory(const GuestMemoryAccess& access) { memory_ = access; }

    [[nodiscard]] ServiceResult<micropixel_surface_create_response_t> Create(
        const micropixel_surface_create_request_t& request);
    [[nodiscard]] ServiceResult<void> Present(const micropixel_surface_present_request_t& request);
    [[nodiscard]] ServiceResult<void> Destroy(micropixel_surface_handle_t surface);

    // Host pause: stop scanning out and return every in-flight buffer.
    void Suspend();
    void Resume();
    // Session teardown: destroys the surface before the event queue closes.
    void Shutdown();

    // Guest task. Host-owned buffer `index` for a raster kernel to write:
    // MICROPIXEL_STATUS_OK and `view_out`; NOT_FOUND when no Host-buffer
    // surface exists or the index is out of range; STALE_STATE while the
    // display may still be reading that buffer.
    [[nodiscard]] int32_t HostBuffer(uint32_t index, HostBufferView& view_out) const;
    // Panel byte order the Host buffers are kept in (from the created surface).
    [[nodiscard]] bool native_byte_swapped() const {  // NOLINT(readability-identifier-naming)
        return native_byte_swapped_;
    }

   private:
    static void OnBufferReleased(void* context, uint8_t buffer_index, uint64_t timestamp_us);
    [[nodiscard]] bool AllocateHostBuffers(uint32_t width, uint32_t height, uint32_t count);
    void FreeHostBuffers();

    device::GraphicsService& graphics_;
    EventQueue& events_;
    int64_t clock_origin_us_{};
    GuestMemoryAccess memory_{};
    micropixel_surface_handle_t handle_{};
    uint32_t next_handle_{1U};
    uint32_t buffer_count_{};
    std::atomic<uint32_t> sequence_{0U};
    std::atomic<bool> created_{false};
    // Bit per buffer between Present and the device's release callback.
    std::atomic<uint32_t> in_flight_mask_{0U};
    // Set while Destroy/Shutdown drain the device: release callbacks still
    // clear in_flight_mask_ but no longer post SURFACE_RELEASED, so the
    // presenter task can never block on a Guest queue nobody is draining.
    std::atomic<bool> retiring_{false};
    // Host-buffer surface: buffer_count_ PSRAM frames, freed after the device
    // has returned every buffer (Destroy/Shutdown).
    bool host_buffers_{};
    bool native_byte_swapped_{};
    uint8_t* host_pixels_[MICROPIXEL_SURFACE_MAX_BUFFERS]{};
    uint32_t host_width_{};
    uint32_t host_height_{};
    uint32_t host_pitch_{};
};

}  // namespace micropixel::runtime

#endif
