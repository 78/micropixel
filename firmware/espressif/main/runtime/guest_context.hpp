#ifndef MICROPIXEL_RUNTIME_GUEST_CONTEXT_HPP
#define MICROPIXEL_RUNTIME_GUEST_CONTEXT_HPP

#include <array>
#include <string_view>

#include "abi/micropixel_abi.h"
#include "device/device_services.hpp"
#include "runtime/abi/service_endpoints.hpp"
#include "runtime/abi/service_registry.hpp"
#include "runtime/audio/audio_playback_service.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/guest_log_sink.hpp"
#include "runtime/key_event_bridge.hpp"
#include "runtime/resources/resource_service.hpp"
#include "runtime/services/storage_service.hpp"
#include "runtime/services/timer_service.hpp"
#include "runtime/touch_event_bridge.hpp"

namespace micropixel::runtime {

class GuestContext final {
   public:
    GuestContext(const micropixel_aot_package_t& package, device::DeviceServices& devices,
                 std::string_view effective_locale, GuestLogSink* log_sink);
    GuestContext(const GuestContext&) = delete;
    GuestContext& operator=(const GuestContext&) = delete;
    ~GuestContext();

    [[nodiscard]] bool Suspend(TickType_t timeout);
    [[nodiscard]] bool Resume();
    [[nodiscard]] bool RequestStop();
    void ForceStop();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return events_.valid() && timers_.valid() && resources_.valid() && audio_playback_.valid() && storage_.valid();
    }
    [[nodiscard]] micropixel_app_time_t AppTimeNow() const { return timers_.Now(); }
    [[nodiscard]] EventWaitResult WaitEvent(micropixel_event_t& event, uint64_t timeout_us) {
        return events_.Wait(event, timeout_us);
    }
    [[nodiscard]] bool PushRequired(const micropixel_event_t& event) { return events_.PushRequired(event); }
    [[nodiscard]] bool PushTouch(const device::TouchSample& sample) { return touch_events_.Push(sample); }
    [[nodiscard]] bool PushKey(const device::KeySample& sample) { return key_events_.Push(sample); }
    void WriteLog(uint32_t level, const uint8_t* bytes, uint32_t length) const;
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
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> LoadTexture(uint32_t asset_id) {
        return resources_.LoadTexture(asset_id);
    }
    [[nodiscard]] ServiceResult<void> ReleaseTexture(micropixel_texture_handle_t texture) {
        return resources_.ReleaseTexture(texture);
    }
    [[nodiscard]] ServiceResult<micropixel_font_info_t> LoadFont(uint32_t resource_id);
    [[nodiscard]] ServiceResult<void> ReleaseFont(micropixel_font_handle_t font);
    [[nodiscard]] ServiceResult<micropixel_text_metrics_t> MeasureText(micropixel_font_handle_t font, const char* text,
                                                                       uint32_t text_length);
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> CreateStreamingTexture(uint32_t width, uint32_t height,
                                                                                  uint32_t pixel_format) {
        return resources_.CreateStreamingTexture(width, height, pixel_format);
    }
    [[nodiscard]] ServiceResult<void> UpdateStreamingTexture(
        const micropixel_streaming_texture_update_request_t& update, const uint8_t* pixels);
    [[nodiscard]] ServiceResult<void> BeginTextureUpdateBatch();
    [[nodiscard]] ServiceResult<void> FinishTextureUpdateBatch();
    [[nodiscard]] bool ResolveTexture(micropixel_texture_handle_t texture, device::BitmapView& view_out) const {
        return resources_.ResolveTexture(texture, view_out);
    }
    [[nodiscard]] device::DeviceResult<void> GraphicsSubmit(const uint8_t* bytes, uint32_t length);
    [[nodiscard]] device::DeviceResult<void> GraphicsBeginFrame();
    [[nodiscard]] device::DeviceResult<void> GraphicsCommitFrame();
    [[nodiscard]] device::DeviceResult<void> GraphicsCancelFrame();
    [[nodiscard]] device::DeviceResult<micropixel_graphics_info_t> GraphicsInfo() const {
        return devices_.graphics().GetInfo();
    }
    [[nodiscard]] device::DeviceResult<micropixel_input_info_t> InputInfo() const { return devices_.input().GetInfo(); }
    [[nodiscard]] device::DeviceResult<micropixel_audio_info_t> AudioInfo() const { return devices_.audio().GetInfo(); }
    [[nodiscard]] device::DeviceResult<uint32_t> RandomU32() const { return devices_.random().U32(); }
    [[nodiscard]] device::DeviceResult<void> AudioPlayTone(const micropixel_audio_tone_t& tone) const {
        return devices_.audio().PlayTone(tone);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_clip_info_t> AudioLoadClip(uint32_t asset_id) {
        return audio_playback_.LoadClip(asset_id);
    }
    [[nodiscard]] ServiceResult<void> AudioReleaseClip(micropixel_audio_clip_handle_t clip) {
        return audio_playback_.ReleaseClip(clip);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_playback_handle_t> AudioStartPlayback(
        const micropixel_audio_playback_start_request_t& request) {
        return audio_playback_.Start(request);
    }
    [[nodiscard]] ServiceResult<void> AudioPausePlayback(micropixel_audio_playback_handle_t playback) {
        return audio_playback_.Pause(playback);
    }
    [[nodiscard]] ServiceResult<void> AudioResumePlayback(micropixel_audio_playback_handle_t playback) {
        return audio_playback_.Resume(playback);
    }
    [[nodiscard]] ServiceResult<void> AudioSetPlaybackVolume(micropixel_audio_playback_handle_t playback,
                                                             uint16_t volume_per_mille) {
        return audio_playback_.SetVolume(playback, volume_per_mille);
    }
    [[nodiscard]] ServiceResult<void> AudioStopPlayback(micropixel_audio_playback_handle_t playback) {
        return audio_playback_.Stop(playback);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_playback_state_response_t> AudioPlaybackState(
        micropixel_audio_playback_handle_t playback) {
        return audio_playback_.State(playback);
    }
    [[nodiscard]] ServiceResult<void> AudioStopAll() { return audio_playback_.StopAll(); }
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
    static bool ResolveTextureForGraphics(void* context, micropixel_texture_handle_t texture,
                                          device::BitmapView& view_out);
    static bool RetainTextureForGraphics(void* context, micropixel_texture_handle_t texture);
    static void ReleaseTextureForGraphics(void* context, micropixel_texture_handle_t texture);
    [[nodiscard]] device::TextureAccess GraphicsTextureAccess();

    device::DeviceServices& devices_;
    GuestLogSink* log_sink_{};
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id_{};
    int64_t clock_origin_us_{};
    EventQueue events_;
    TimerService timers_;
    ResourceService resources_;
    AudioPlaybackService audio_playback_;
    StorageService storage_;
    TouchEventBridge touch_events_;
    KeyEventBridge key_events_;
    TimerServiceEndpoint timer_endpoint_;
    SystemServiceEndpoint system_endpoint_;
    StorageServiceEndpoint storage_endpoint_;
    ResourceServiceEndpoint resource_endpoint_;
    RandomServiceEndpoint random_endpoint_;
    GraphicsServiceEndpoint graphics_endpoint_;
    InputServiceEndpoint input_endpoint_;
    AudioServiceEndpoint audio_endpoint_;
    ServiceRegistry service_registry_;
    uint32_t core_sequence_{};
    bool suspended_{};
};

}  // namespace micropixel::runtime

#endif
