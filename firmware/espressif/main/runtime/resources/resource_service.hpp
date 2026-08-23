#ifndef MICROPIXEL_RUNTIME_RESOURCES_RESOURCE_SERVICE_HPP
#define MICROPIXEL_RUNTIME_RESOURCES_RESOURCE_SERVICE_HPP

#include <atomic>
#include <cstdint>

#include "device/graphics.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "runtime/bundle/bundle_reader.h"
#include "runtime/event_queue.hpp"
#include "runtime/resources/bitmap_store.hpp"
#include "runtime/runtime_limits.hpp"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class ResourceService final {
   public:
    ResourceService(const micropixel_aot_package_t& package, EventQueue& events, int64_t clock_origin_us);
    ResourceService(const ResourceService&) = delete;
    ResourceService& operator=(const ResourceService&) = delete;
    ~ResourceService();

    [[nodiscard]] bool valid() const;  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ServiceResult<micropixel_load_request_handle_t> Load(uint32_t asset_id);
    [[nodiscard]] ServiceResult<void> Cancel(micropixel_load_request_handle_t request);
    [[nodiscard]] ServiceResult<micropixel_bitmap_info_t> BitmapInfo(micropixel_bitmap_handle_t bitmap);
    [[nodiscard]] ServiceResult<void> BitmapRelease(micropixel_bitmap_handle_t bitmap);
    [[nodiscard]] ServiceResult<micropixel_bitmap_handle_t> CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                                                   uint32_t pixel_format);
    [[nodiscard]] ServiceResult<device::BitmapView> MutableBitmap(micropixel_bitmap_handle_t bitmap) const;
    [[nodiscard]] bool ResolveBitmap(micropixel_bitmap_handle_t bitmap, device::BitmapView& view_out) const;
    void Shutdown();

   private:
    struct Work final {
        uint32_t request{};
        micropixel_bundle_asset_view_t asset{};
    };

    struct RequestSlot final {
        uint32_t handle{};
    };

    static void WorkerEntry(void* argument);
    void WorkerLoop();
    void Process(const Work& work);
    [[nodiscard]] uint32_t AllocateRequestLocked();
    [[nodiscard]] bool RequestPendingLocked(uint32_t request) const;
    void ClearRequestLocked(uint32_t request);
    void PostCompletion(uint32_t request, uint32_t bitmap, int32_t status);

    // Non-owning descriptor copy. AotPackage remains the mapping owner for the
    // complete AppSession, while this value remains stable if that owner moves.
    micropixel_aot_package_t package_{};
    EventQueue& events_;
    int64_t clock_origin_us_{};
    QueueHandle_t work_queue_{};
    SemaphoreHandle_t worker_stopped_{};
    TaskHandle_t worker_{};
    mutable portMUX_TYPE request_lock_ = portMUX_INITIALIZER_UNLOCKED;
    RequestSlot requests_[limits::kMaxResourceRequests]{};
    uint32_t next_request_{1U};
    BitmapStore bitmaps_;
    std::atomic<bool> stopping_{};
    bool shutdown_complete_{};
};

}  // namespace micropixel::runtime

#endif
