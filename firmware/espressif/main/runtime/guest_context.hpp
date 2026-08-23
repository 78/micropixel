#ifndef MICROPIXEL_RUNTIME_GUEST_CONTEXT_HPP
#define MICROPIXEL_RUNTIME_GUEST_CONTEXT_HPP

#include "abi/micropixel_abi.h"
#include "device/device_services.hpp"
#include "runtime/abi/service_endpoints.hpp"
#include "runtime/abi/service_registry.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/resources/resource_service.hpp"
#include "runtime/services/storage_service.hpp"
#include "runtime/services/timer_service.hpp"
#include "runtime/touch_event_bridge.hpp"

namespace micropixel::runtime {

class GuestContext final {
   public:
    GuestContext(const micropixel_aot_package_t& package, device::DeviceServices& devices);
    GuestContext(const GuestContext&) = delete;
    GuestContext& operator=(const GuestContext&) = delete;
    ~GuestContext();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return events_.valid() && timers_.valid() && resources_.valid() && storage_.valid();
    }
    [[nodiscard]] micropixel_app_time_t AppTimeNow() const { return timers_.Now(); }
    [[nodiscard]] EventWaitResult WaitEvent(micropixel_event_t& event, uint64_t timeout_us) {
        return events_.Wait(event, timeout_us);
    }
    [[nodiscard]] bool PushRequired(const micropixel_event_t& event) { return events_.PushRequired(event); }
    [[nodiscard]] bool PushTouch(const device::TouchSample& sample) { return touch_events_.Push(sample); }
    void NoteEventDelivered(const micropixel_event_t& event) { touch_events_.NoteDelivered(event); }
    [[nodiscard]] int32_t ServiceOpen(uint32_t service_id, uint32_t required_interface_version,
                                      micropixel_service_info_t& info_out, uint32_t info_capacity) const {
        return service_registry_.Open(service_id, required_interface_version, info_out, info_capacity);
    }
    [[nodiscard]] int32_t ServiceCall(micropixel_service_handle_t service, uint32_t method_id, const uint8_t* request,
                                      uint32_t request_size, uint8_t* response, uint32_t response_capacity,
                                      uint32_t& response_size_out) const {
        return service_registry_.Call(service, method_id, request, request_size, response, response_capacity,
                                      response_size_out);
    }
    [[nodiscard]] int32_t ServiceSubmit(micropixel_service_handle_t service, uint32_t channel_id, const uint8_t* bytes,
                                        uint32_t length) const {
        return service_registry_.Submit(service, channel_id, bytes, length);
    }

    [[nodiscard]] ServiceResult<micropixel_timer_handle_t> TimerCreate() { return timers_.Create(); }
    [[nodiscard]] ServiceResult<void> TimerStart(micropixel_timer_handle_t handle, uint64_t initial_delay_us,
                                                 uint64_t period_us) {
        return timers_.Start(handle, initial_delay_us, period_us);
    }
    [[nodiscard]] ServiceResult<void> TimerCancel(micropixel_timer_handle_t handle) { return timers_.Cancel(handle); }
    [[nodiscard]] ServiceResult<void> TimerRelease(micropixel_timer_handle_t handle) { return timers_.Release(handle); }
    [[nodiscard]] ServiceResult<micropixel_load_request_handle_t> AssetLoad(uint32_t asset_id) {
        return resources_.Load(asset_id);
    }
    [[nodiscard]] ServiceResult<void> AssetRequestCancel(micropixel_load_request_handle_t request) {
        return resources_.Cancel(request);
    }
    [[nodiscard]] ServiceResult<micropixel_bitmap_info_t> BitmapInfo(micropixel_bitmap_handle_t bitmap) {
        return resources_.BitmapInfo(bitmap);
    }
    [[nodiscard]] ServiceResult<void> BitmapRelease(micropixel_bitmap_handle_t bitmap) {
        return resources_.BitmapRelease(bitmap);
    }
    [[nodiscard]] ServiceResult<micropixel_bitmap_handle_t> CreateOffscreenSurface(uint32_t width, uint32_t height,
                                                                                   uint32_t pixel_format) {
        return resources_.CreateOffscreenSurface(width, height, pixel_format);
    }
    [[nodiscard]] ServiceResult<void> UpdateOffscreenSurface(
        const micropixel_offscreen_surface_update_request_t& update, const uint8_t* pixels);
    [[nodiscard]] ServiceResult<void> BeginOffscreenUpdateFrame();
    [[nodiscard]] ServiceResult<void> CommitOffscreenUpdateFrame();
    [[nodiscard]] bool ResolveBitmap(micropixel_bitmap_handle_t bitmap, device::BitmapView& view_out) const {
        return resources_.ResolveBitmap(bitmap, view_out);
    }
    [[nodiscard]] device::DeviceResult<void> GraphicsSubmit(const uint8_t* bytes, uint32_t length);
    [[nodiscard]] device::DeviceResult<void> GraphicsBeginFrame();
    [[nodiscard]] device::DeviceResult<void> GraphicsCommitFrame();
    [[nodiscard]] device::DeviceResult<micropixel_graphics_info_t> GraphicsInfo() const {
        return devices_.graphics().GetInfo();
    }
    [[nodiscard]] device::DeviceResult<micropixel_input_info_t> InputInfo() const { return devices_.input().GetInfo(); }
    [[nodiscard]] device::DeviceResult<micropixel_audio_info_t> AudioInfo() const { return devices_.audio().GetInfo(); }
    [[nodiscard]] device::DeviceResult<uint32_t> RandomU32() const { return devices_.random().U32(); }
    [[nodiscard]] device::DeviceResult<void> AudioPlayTone(const micropixel_audio_tone_t& tone) const {
        return devices_.audio().PlayTone(tone);
    }
    [[nodiscard]] device::DeviceResult<void> AudioStopAll() const { return devices_.audio().StopAll(); }
    [[nodiscard]] ServiceResult<uint32_t> KvGetU32(const char* key, uint32_t key_length) {
        return storage_.GetU32(key, key_length);
    }
    [[nodiscard]] ServiceResult<void> KvSetU32(const char* key, uint32_t key_length, uint32_t value) {
        return storage_.SetU32(key, key_length, value);
    }
    [[nodiscard]] ServiceResult<bool> KvGetBool(const char* key, uint32_t key_length) {
        return storage_.GetBool(key, key_length);
    }
    [[nodiscard]] ServiceResult<void> KvSetBool(const char* key, uint32_t key_length, bool value) {
        return storage_.SetBool(key, key_length, value);
    }
    [[nodiscard]] ServiceResult<uint32_t> KvGetBytes(const char* key, uint32_t key_length, uint8_t* bytes_out,
                                                     uint32_t capacity) {
        return storage_.GetBytes(key, key_length, bytes_out, capacity);
    }
    [[nodiscard]] ServiceResult<void> KvSetBytes(const char* key, uint32_t key_length, const uint8_t* bytes,
                                                 uint32_t length) {
        return storage_.SetBytes(key, key_length, bytes, length);
    }
    [[nodiscard]] ServiceResult<void> KvRemove(const char* key, uint32_t key_length) {
        return storage_.Remove(key, key_length);
    }

   private:
    static bool ResolveBitmapForGraphics(void* context, micropixel_bitmap_handle_t bitmap,
                                         device::BitmapView& view_out);

    device::DeviceServices& devices_;
    int64_t clock_origin_us_{};
    EventQueue events_;
    TimerService timers_;
    ResourceService resources_;
    StorageService storage_;
    TouchEventBridge touch_events_;
    TimerServiceEndpoint timer_endpoint_;
    StorageServiceEndpoint storage_endpoint_;
    ResourceServiceEndpoint resource_endpoint_;
    RandomServiceEndpoint random_endpoint_;
    GraphicsServiceEndpoint graphics_endpoint_;
    InputServiceEndpoint input_endpoint_;
    AudioServiceEndpoint audio_endpoint_;
    ServiceRegistry service_registry_;
};

}  // namespace micropixel::runtime

#endif
