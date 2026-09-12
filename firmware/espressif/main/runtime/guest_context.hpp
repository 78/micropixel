#ifndef MICROPIXEL_RUNTIME_GUEST_CONTEXT_HPP
#define MICROPIXEL_RUNTIME_GUEST_CONTEXT_HPP

#include <array>
#include <string_view>

#include "abi/micropixel_abi.h"
#include "device/device_services.hpp"
#include "runtime/abi/service_endpoints.hpp"
#include "runtime/abi/service_registry.hpp"
#include "runtime/audio/audio_playback_service.hpp"
#include "runtime/audio/pcm_stream_service.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/guest_log_sink.hpp"
#include "runtime/key_event_bridge.hpp"
#include "runtime/resources/resource_service.hpp"
#include "runtime/services/direct_surface_service.hpp"
#include "runtime/services/gpio_service.hpp"
#include "runtime/services/haptics_service.hpp"
#include "runtime/services/raster_service.hpp"
#include "runtime/services/sensor_service.hpp"
#include "runtime/services/storage_service.hpp"
#include "runtime/services/timer_service.hpp"
#include "runtime/touch_event_bridge.hpp"

namespace micropixel::work {
class BackgroundExecutor;
}

namespace micropixel::runtime {

// Widens canonical RGB565 by replicating the top bits, matching the SDK's
// Color::FromRgb565 so a raster TEXT color renders like the Scene label color.
[[nodiscard]] constexpr uint32_t Rgb565ToRgb888(uint16_t color) {
    const uint32_t r5 = color >> 11U;
    const uint32_t g6 = (color >> 5U) & 0x3FU;
    const uint32_t b5 = color & 0x1FU;
    return (((r5 << 3U) | (r5 >> 2U)) << 16U) | (((g6 << 2U) | (g6 >> 4U)) << 8U) | ((b5 << 3U) | (b5 >> 2U));
}

class GuestContext final {
   public:
    GuestContext(const micropixel_aot_package_t& package, device::DeviceServices& devices,
                 work::BackgroundExecutor& background_executor, std::string_view effective_locale,
                 const micropixel_system_launch_arguments_response_t& launch_arguments, GuestLogSink* log_sink);
    GuestContext(const GuestContext&) = delete;
    GuestContext& operator=(const GuestContext&) = delete;
    ~GuestContext();

    [[nodiscard]] bool Suspend(TickType_t timeout);
    [[nodiscard]] bool Resume();
    [[nodiscard]] bool RequestStop();
    void ForceStop();

    [[nodiscard]] bool valid() const {  // NOLINT(readability-identifier-naming)
        return events_.valid() && timers_.valid() && sensors_.valid() && gpio_.valid() && haptics_.valid() &&
               resources_.valid() && audio_playback_.valid() && storage_.valid() && audio_foreground_ready_;
    }
    [[nodiscard]] micropixel_app_time_t AppTimeNow() const { return timers_.Now(); }
    [[nodiscard]] EventWaitResult WaitEvent(micropixel_event_t& event, uint64_t timeout_us) {
        return events_.Wait(event, timeout_us);
    }
    [[nodiscard]] bool PushRequired(const micropixel_event_t& event) { return events_.PushRequired(event); }
    [[nodiscard]] bool PushTouch(const device::TouchSample& sample) { return touch_events_.Push(sample); }
    [[nodiscard]] bool PushKey(const device::KeySample& sample) { return key_events_.Push(sample); }
    void WriteLog(uint32_t level, const uint8_t* bytes, uint32_t length);
    // Last Guest log line that began with "panic: " (the SDK's single-line panic
    // report), or an empty string. Attached to the trap failure detail so the
    // developer sees the cause instead of only "Exception: unreachable".
    [[nodiscard]] const char* LastPanic() const { return last_panic_.data(); }
    [[nodiscard]] const char* LastDecodeFailure() const { return resources_.LastDecodeFailure(); }
    void NoteEventDelivered(const micropixel_event_t& event) { touch_events_.NoteDelivered(event); }
    [[nodiscard]] int32_t ServiceOpen(uint32_t service_id, uint32_t required_interface_version,
                                      micropixel_service_info_t& info_out, uint32_t info_capacity) const {
        return service_registry_.Open(service_id, required_interface_version, info_out, info_capacity);
    }
    [[nodiscard]] int32_t ServiceCall(micropixel_service_handle_t service_handle, uint32_t method_id,
                                      const uint8_t* request, uint32_t request_size, uint8_t* response,
                                      uint32_t response_capacity, uint32_t& response_size_out) const {
        return service_registry_.Call(service_handle, method_id, request, request_size, response, response_capacity,
                                      response_size_out);
    }
    [[nodiscard]] int32_t ServiceSubmit(micropixel_service_handle_t service_handle, uint32_t channel_id,
                                        const uint8_t* bytes, uint32_t length) const {
        return service_registry_.Submit(service_handle, channel_id, bytes, length);
    }

    [[nodiscard]] ServiceResult<micropixel_timer_handle_t> TimerCreate() { return timers_.Create(); }
    [[nodiscard]] ServiceResult<void> TimerStart(micropixel_timer_handle_t handle, uint64_t initial_delay_us,
                                                 uint64_t period_us) {
        return timers_.Start(handle, initial_delay_us, period_us);
    }
    [[nodiscard]] ServiceResult<void> TimerCancel(micropixel_timer_handle_t handle) { return timers_.Cancel(handle); }
    [[nodiscard]] ServiceResult<void> TimerRelease(micropixel_timer_handle_t handle) { return timers_.Release(handle); }
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> LoadTexture(uint32_t asset_id, uint32_t scale_numerator,
                                                                       uint32_t scale_denominator) {
        return resources_.LoadTexture(asset_id, scale_numerator, scale_denominator);
    }
    [[nodiscard]] ServiceResult<void> ReleaseTexture(micropixel_texture_handle_t texture_handle) {
        return resources_.ReleaseTexture(texture_handle);
    }
    [[nodiscard]] ServiceResult<micropixel_font_info_t> LoadFont(uint32_t resource_id);
    [[nodiscard]] ServiceResult<void> ReleaseFont(micropixel_font_handle_t font_handle);
    [[nodiscard]] ServiceResult<micropixel_text_metrics_t> MeasureText(micropixel_font_handle_t font_handle,
                                                                       const char* text, uint32_t text_length);
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> CreateDynamicTexture(
        const micropixel_dynamic_texture_create_request_t& request) {
        return resources_.CreateDynamicTexture(request);
    }
    [[nodiscard]] ServiceResult<micropixel_texture_info_t> UpdateDynamicTexture(
        const micropixel_dynamic_texture_update_request_t& request) {
        return resources_.UpdateDynamicTexture(request);
    }
    [[nodiscard]] bool ResolveTexture(micropixel_texture_handle_t texture_handle, device::BitmapView& view_out) const {
        return resources_.ResolveTexture(texture_handle, view_out);
    }
    [[nodiscard]] device::DeviceResult<void> GraphicsSubmit(const uint8_t* bytes, uint32_t length);
    // Direct Surface (Graphics 1.5) and raster kernels (Graphics 1.6). Guest
    // offsets are resolved through the memory access bound by the session
    // after instantiation.
    void BindGuestMemory(const GuestMemoryAccess& access) {
        resources_.BindGuestMemory(access);
        direct_surface_.BindGuestMemory(access);
        raster_.BindGuestMemory(access);
    }
    [[nodiscard]] ServiceResult<micropixel_surface_create_response_t> SurfaceCreate(
        const micropixel_surface_create_request_t& request) {
        auto created = direct_surface_.Create(request);
        // Textures loaded from now on are stored in the surface's pixel format
        // and byte order so raster IMAGE records copy them verbatim.
        if (created) {
            resources_.SetPreferredRasterTarget(created->native_pixel_format, direct_surface_.native_byte_swapped());
        }
        return created;
    }
    [[nodiscard]] ServiceResult<void> SurfacePresent(const micropixel_surface_present_request_t& request) {
        return direct_surface_.Present(request);
    }
    [[nodiscard]] ServiceResult<void> SurfaceDestroy(micropixel_surface_handle_t surface_handle) {
        return direct_surface_.Destroy(surface_handle);
    }
    // Device info plus the runtime-owned raster pool fields.
    [[nodiscard]] device::DeviceResult<micropixel_graphics_info_t> GraphicsInfo() const;
    [[nodiscard]] bool RasterAvailable() const { return raster_.available(); }
    [[nodiscard]] ServiceResult<void> RasterTextureUpload(const micropixel_raster_texture_upload_request_t& request) {
        return raster_.UploadTexture(request);
    }
    [[nodiscard]] ServiceResult<void> RasterPaletteUpload(const micropixel_raster_palette_upload_request_t& request) {
        return raster_.UploadPalette(request);
    }
    [[nodiscard]] ServiceResult<void> RasterWarpUpload(const micropixel_raster_warp_upload_request_t& request) {
        return raster_.UploadWarp(request);
    }
    [[nodiscard]] ServiceResult<void> RasterSubmit(const uint8_t* bytes, uint32_t length) {
        // TEXT records reuse the Graphics device's font handles: MeasureText
        // doubles as the validity check (handle resolves, UTF-8 lays out) and
        // DrawText paints into the Host-owned buffer on this task.
        const raster::TextBinding text{
            .validate =
                [](void* context, micropixel_font_handle_t font_handle, const char* string, uint32_t text_length) {
                    return static_cast<bool>(
                        static_cast<GuestContext*>(context)->MeasureText(font_handle, string, text_length));
                },
            .draw =
                [](void* context, const raster::Target& target, int32_t x, int32_t y, uint16_t color,
                   micropixel_font_handle_t font_handle, const char* string, uint32_t text_length) {
                    const device::TextTarget destination{.pixels = target.pixels,
                                                         .width = target.width,
                                                         .height = target.height,
                                                         .pitch = target.pitch,
                                                         .byte_swapped = target.byte_swapped};
                    return static_cast<bool>(static_cast<GuestContext*>(context)->devices_.graphics().DrawText(
                        destination, x, y, Rgb565ToRgb888(color), font_handle, string, text_length));
                },
            .context = this,
        };
        const raster::CopyBinding copy{
            .copy_blocks =
                [](void* context, const raster::Target& target, const device::OpaqueCopyBlock* blocks, uint32_t count) {
                    auto* self = static_cast<GuestContext*>(context);
                    const device::PixelTarget destination{.pixels = target.pixels,
                                                          .width = target.width,
                                                          .height = target.height,
                                                          .pitch = target.pitch,
                                                          .byte_swapped = target.byte_swapped};
                    return static_cast<bool>(self->devices_.graphics().CopyOpaqueBlocks(destination, blocks, count));
                },
            .context = this,
        };
        return raster_.Submit(
            bytes, length, direct_surface_,
            [](void* context, uint32_t handle, device::BitmapView& view) {
                auto* self = static_cast<GuestContext*>(context);
                return self->resources_.ResolveTextureForRaster(handle, self->direct_surface_.native_byte_swapped(),
                                                                view);
            },
            this, text, copy);
    }
    [[nodiscard]] device::DeviceResult<micropixel_input_info_t> InputInfo() const { return devices_.input().GetInfo(); }
    [[nodiscard]] device::DeviceResult<micropixel_audio_info_t> AudioInfo() const { return devices_.audio().GetInfo(); }
    [[nodiscard]] device::DeviceResult<uint32_t> RandomU32() const { return devices_.random().U32(); }
    [[nodiscard]] device::DeviceResult<micropixel_devices_list_response_t> DevicesList(uint16_t kind,
                                                                                       uint16_t first_index) const {
        return devices_.devices().List(kind, first_index);
    }
    [[nodiscard]] device::DeviceResult<micropixel_device_info_t> DeviceInfo(micropixel_device_id_t device) const {
        return devices_.devices().GetInfo(device);
    }
    [[nodiscard]] device::DeviceResult<micropixel_sensor_info_t> SensorInfo(micropixel_device_id_t device) const {
        return devices_.sensors().GetInfo(device);
    }
    [[nodiscard]] ServiceResult<micropixel_sensor_open_response_t> SensorOpen(micropixel_device_id_t device,
                                                                              uint16_t expected_kind) {
        return sensors_.Open(device, expected_kind);
    }
    [[nodiscard]] ServiceResult<micropixel_sensor_reading_t> SensorRead(micropixel_sensor_handle_t sensor_handle) {
        return sensors_.Read(sensor_handle);
    }
    [[nodiscard]] ServiceResult<void> SensorSetSampleInterval(micropixel_sensor_handle_t sensor_handle,
                                                              uint64_t interval_us) {
        return sensors_.SetSampleInterval(sensor_handle, interval_us);
    }
    [[nodiscard]] ServiceResult<void> SensorRelease(micropixel_sensor_handle_t sensor_handle) {
        return sensors_.Release(sensor_handle);
    }
    [[nodiscard]] device::DeviceResult<micropixel_gpio_info_t> GpioInfo(micropixel_device_id_t device) const {
        return devices_.gpio().GetInfo(device);
    }
    [[nodiscard]] ServiceResult<micropixel_gpio_open_response_t> GpioOpen(
        const micropixel_gpio_open_request_t& request) {
        return gpio_.Open(request);
    }
    [[nodiscard]] ServiceResult<micropixel_gpio_value_response_t> GpioRead(micropixel_gpio_handle_t gpio_handle) {
        return gpio_.Read(gpio_handle);
    }
    [[nodiscard]] ServiceResult<void> GpioWrite(micropixel_gpio_handle_t gpio_handle, bool value) {
        return gpio_.Write(gpio_handle, value);
    }
    [[nodiscard]] ServiceResult<void> GpioSetPwmDuty(micropixel_gpio_handle_t gpio_handle, uint16_t duty_per_mille) {
        return gpio_.SetPwmDuty(gpio_handle, duty_per_mille);
    }
    [[nodiscard]] ServiceResult<void> GpioRelease(micropixel_gpio_handle_t gpio_handle) {
        return gpio_.Release(gpio_handle);
    }
    [[nodiscard]] device::DeviceResult<micropixel_haptics_info_t> HapticsInfo(micropixel_device_id_t device) const {
        return devices_.haptics().GetInfo(device);
    }
    [[nodiscard]] ServiceResult<micropixel_handle_response_t> HapticsOpen(micropixel_device_id_t device) {
        return haptics_.Open(device);
    }
    [[nodiscard]] ServiceResult<void> HapticsPlay(const micropixel_haptics_play_request_t& request) {
        return haptics_.Play(request);
    }
    [[nodiscard]] ServiceResult<void> HapticsStop(micropixel_haptics_handle_t haptics_handle) {
        return haptics_.Stop(haptics_handle);
    }
    [[nodiscard]] ServiceResult<void> HapticsRelease(micropixel_haptics_handle_t haptics_handle) {
        return haptics_.Release(haptics_handle);
    }
    [[nodiscard]] device::DeviceResult<micropixel_power_info_t> PowerInfo(micropixel_device_id_t device) {
        return devices_.power_info().Get(device);
    }
    [[nodiscard]] device::DeviceResult<void> AudioPlayTone(const micropixel_audio_tone_t& tone) const {
        return devices_.audio().PlayTone(tone);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_clip_info_t> AudioLoadClip(uint32_t asset_id) {
        return audio_playback_.LoadClip(asset_id);
    }
    [[nodiscard]] ServiceResult<void> AudioReleaseClip(micropixel_audio_clip_handle_t clip_handle) {
        return audio_playback_.ReleaseClip(clip_handle);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_playback_handle_t> AudioStartPlayback(
        const micropixel_audio_playback_start_request_t& request) {
        return audio_playback_.Start(request);
    }
    [[nodiscard]] ServiceResult<void> AudioPausePlayback(micropixel_audio_playback_handle_t playback_handle) {
        return audio_playback_.Pause(playback_handle);
    }
    [[nodiscard]] ServiceResult<void> AudioResumePlayback(micropixel_audio_playback_handle_t playback_handle) {
        return audio_playback_.Resume(playback_handle);
    }
    [[nodiscard]] ServiceResult<void> AudioSetPlaybackVolume(micropixel_audio_playback_handle_t playback_handle,
                                                             uint16_t volume_per_mille) {
        return audio_playback_.SetVolume(playback_handle, volume_per_mille);
    }
    [[nodiscard]] ServiceResult<void> AudioStopPlayback(micropixel_audio_playback_handle_t playback_handle) {
        return audio_playback_.Stop(playback_handle);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_playback_state_response_t> AudioPlaybackState(
        micropixel_audio_playback_handle_t playback_handle) {
        return audio_playback_.State(playback_handle);
    }
    [[nodiscard]] ServiceResult<void> AudioStopAll() {
        pcm_stream_.CloseAll();
        return audio_playback_.StopAll();
    }
    [[nodiscard]] ServiceResult<micropixel_audio_pcm_stream_open_response_t> AudioOpenPcmStream(
        const micropixel_audio_pcm_stream_open_request_t& request) {
        return pcm_stream_.Open(request);
    }
    [[nodiscard]] ServiceResult<micropixel_audio_pcm_stream_write_response_t> AudioWritePcmStream(
        const micropixel_audio_pcm_stream_write_request_t& request, const int16_t* samples, uint32_t payload_bytes) {
        return pcm_stream_.Write(request, samples, payload_bytes);
    }
    [[nodiscard]] ServiceResult<void> AudioClosePcmStream(micropixel_audio_pcm_stream_handle_t stream_handle) {
        return pcm_stream_.Close(stream_handle);
    }
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
    static bool ResolveTextureForGraphics(void* context, micropixel_texture_handle_t texture_handle,
                                          device::BitmapView& view_out);
    static bool RetainTextureForGraphics(void* context, micropixel_texture_handle_t texture_handle);
    static void ReleaseTextureForGraphics(void* context, micropixel_texture_handle_t texture_handle);
    [[nodiscard]] device::TextureAccess GraphicsTextureAccess();

    device::DeviceServices& devices_;
    GuestLogSink* log_sink_{};
    std::array<char, 160U> last_panic_{};
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id_{};
    int64_t clock_origin_us_{};
    EventQueue events_;
    TimerService timers_;
    SensorService sensors_;
    GpioService gpio_;
    HapticsService haptics_;
    ResourceService resources_;
    AudioPlaybackService audio_playback_;
    PcmStreamService pcm_stream_;
    DirectSurfaceService direct_surface_;
    RasterService raster_;
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
    DevicesServiceEndpoint devices_endpoint_;
    SensorsServiceEndpoint sensors_endpoint_;
    GpioServiceEndpoint gpio_endpoint_;
    HapticsServiceEndpoint haptics_endpoint_;
    PowerInfoServiceEndpoint power_info_endpoint_;
    ServiceRegistry service_registry_;
    uint32_t core_sequence_{};
    bool suspended_{};
    bool audio_foreground_ready_{};
};

}  // namespace micropixel::runtime

#endif
