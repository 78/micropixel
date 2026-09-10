#include "device/device_services.hpp"

namespace micropixel::device {
namespace {

template <typename Value>
DeviceResult<Value> Fail(int32_t status) {
    return std::unexpected(DeviceFailure{status});
}

DeviceResult<void> StatusResult(int32_t status) {
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<void>(status);
    }
    return {};
}

}  // namespace

DeviceResult<micropixel_devices_list_response_t> DevicesService::List(uint16_t kind, uint16_t first_index) const {
    if (kind > MICROPIXEL_DEVICE_KIND_NETWORK) {
        return Fail<micropixel_devices_list_response_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t count = implementation_.Count();
    if (count > UINT16_MAX) {
        return Fail<micropixel_devices_list_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    micropixel_devices_list_response_t response{};
    response.size = sizeof(response);
    response.generation = implementation_.Generation();
    uint32_t matched = 0U;
    for (uint32_t index = 0U; index < count; ++index) {
        micropixel_device_info_t info{};
        const int32_t status = implementation_.GetByIndex(index, info);
        if (status != MICROPIXEL_STATUS_OK || info.device == 0U || info.kind == MICROPIXEL_DEVICE_KIND_ANY ||
            info.kind > MICROPIXEL_DEVICE_KIND_NETWORK) {
            const int32_t failure =
                status == MICROPIXEL_STATUS_OK ? static_cast<int32_t>(MICROPIXEL_STATUS_INTERNAL) : status;
            return Fail<micropixel_devices_list_response_t>(failure);
        }
        if (kind != MICROPIXEL_DEVICE_KIND_ANY && info.kind != kind) {
            continue;
        }
        if (matched >= first_index && response.count < MICROPIXEL_DEVICES_LIST_PAGE_SIZE) {
            response.devices[response.count++] = info.device;
        }
        ++matched;
    }
    response.total_count = static_cast<uint16_t>(matched);
    return response;
}

DeviceResult<micropixel_device_info_t> DevicesService::GetInfo(micropixel_device_id_t device) const {
    if (device == 0U) {
        return Fail<micropixel_device_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    micropixel_device_info_t info{};
    const int32_t status = implementation_.GetById(device, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_device_info_t>{info}
                                          : Fail<micropixel_device_info_t>(status);
}

DeviceResult<micropixel_sensor_info_t> SensorsService::GetInfo(micropixel_device_id_t device) const {
    micropixel_sensor_info_t info{};
    const int32_t status = implementation_.GetInfo(device, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_sensor_info_t>{info}
                                          : Fail<micropixel_sensor_info_t>(status);
}

DeviceResult<void> SensorsService::Start(micropixel_device_id_t device, uint32_t interval_us) const {
    return StatusResult(implementation_.Start(device, interval_us));
}

DeviceResult<SensorValues> SensorsService::Read(micropixel_device_id_t device) const {
    SensorValues values{};
    const int32_t status = implementation_.Read(device, values);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<SensorValues>{values} : Fail<SensorValues>(status);
}

void SensorsService::Stop(micropixel_device_id_t device) const { implementation_.StopSampling(device); }

DeviceResult<micropixel_gpio_info_t> GpioService::GetInfo(micropixel_device_id_t device) const {
    micropixel_gpio_info_t info{};
    const int32_t status = implementation_.GetInfo(device, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_gpio_info_t>{info}
                                          : Fail<micropixel_gpio_info_t>(status);
}

DeviceResult<void> GpioService::Open(micropixel_device_id_t device, uint16_t mode, uint16_t pull, uint16_t edge,
                                     uint32_t initial_value, uint32_t pwm_frequency_hz, GpioEdgeSink edge_sink,
                                     void* edge_context) const {
    return StatusResult(
        implementation_.Open(device, mode, pull, edge, initial_value, pwm_frequency_hz, edge_sink, edge_context));
}

DeviceResult<bool> GpioService::Read(micropixel_device_id_t device) const {
    bool value = false;
    const int32_t status = implementation_.Read(device, value);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<bool>{value} : Fail<bool>(status);
}

DeviceResult<void> GpioService::Write(micropixel_device_id_t device, bool value) const {
    return StatusResult(implementation_.Write(device, value));
}

DeviceResult<void> GpioService::SetPwmDuty(micropixel_device_id_t device, uint16_t duty_per_mille) const {
    return StatusResult(implementation_.SetPwmDuty(device, duty_per_mille));
}

void GpioService::SuspendEvents() const { implementation_.SuspendEvents(); }

DeviceResult<void> GpioService::ResumeEvents() const { return StatusResult(implementation_.ResumeEvents()); }

void GpioService::Close(micropixel_device_id_t device) const { implementation_.Close(device); }

DeviceResult<micropixel_haptics_info_t> HapticsService::GetInfo(micropixel_device_id_t device) const {
    micropixel_haptics_info_t info{};
    const int32_t status = implementation_.GetInfo(device, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_haptics_info_t>{info}
                                          : Fail<micropixel_haptics_info_t>(status);
}

DeviceResult<void> HapticsService::Play(micropixel_device_id_t device, uint16_t strength_per_mille,
                                        uint32_t duration_ms) const {
    return StatusResult(implementation_.Play(device, strength_per_mille, duration_ms));
}

DeviceResult<void> HapticsService::Stop(micropixel_device_id_t device) const {
    return StatusResult(implementation_.Stop(device));
}

void HapticsService::SetCompletionSink(HapticCompletionSink sink, void* context) const {
    implementation_.SetCompletionSink(sink, context);
}

DeviceResult<micropixel_power_info_t> PowerInfoService::Get(micropixel_device_id_t device) {
    micropixel_device_info_t device_info{};
    const int32_t device_status = devices_.GetById(device, device_info);
    if (device_status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_power_info_t>(device_status);
    }
    if (device_info.kind != MICROPIXEL_DEVICE_KIND_POWER) {
        return Fail<micropixel_power_info_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    const BatterySnapshot snapshot = battery_.Snapshot();
    micropixel_power_info_t response{};
    response.size = sizeof(response);
    response.device = device;
    if (snapshot.external_power_available && snapshot.external_power_connected) {
        response.source = MICROPIXEL_POWER_SOURCE_EXTERNAL;
    } else if (snapshot.available) {
        response.source = MICROPIXEL_POWER_SOURCE_BATTERY;
    } else {
        response.source = MICROPIXEL_POWER_SOURCE_UNKNOWN;
    }
    if (snapshot.available) {
        response.flags |= MICROPIXEL_POWER_STATE_HAS_BATTERY;
        response.battery_percent = snapshot.percent;
    }
    if (snapshot.charging_available && snapshot.charging) {
        response.flags |= MICROPIXEL_POWER_STATE_CHARGING;
    }
    if (snapshot.charging_available && snapshot.discharging) {
        response.flags |= MICROPIXEL_POWER_STATE_DISCHARGING;
    }
    if (snapshot.external_power_available && snapshot.external_power_connected) {
        response.flags |= MICROPIXEL_POWER_STATE_EXTERNAL_CONNECTED;
    }
    return response;
}

DeviceResult<micropixel_graphics_info_t> GraphicsService::GetInfo() const {
    micropixel_graphics_info_t info{};
    int32_t status = implementation_.GetInfo(info);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_graphics_info_t>(status);
    }
    const DisplaySafeAreaInsets& safe_area = display_.safe_area;
    const uint32_t horizontal_insets = safe_area.left_pixels + safe_area.right_pixels;
    const uint32_t vertical_insets = safe_area.top_pixels + safe_area.bottom_pixels;
    if (info.width != display_.width_pixels || info.height != display_.height_pixels ||
        safe_area.top_pixels > UINT16_MAX || safe_area.right_pixels > UINT16_MAX ||
        safe_area.bottom_pixels > UINT16_MAX || safe_area.left_pixels > UINT16_MAX || horizontal_insets >= info.width ||
        vertical_insets >= info.height) {
        return Fail<micropixel_graphics_info_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    info.safe_inset_top = static_cast<uint16_t>(safe_area.top_pixels);
    info.safe_inset_right = static_cast<uint16_t>(safe_area.right_pixels);
    info.safe_inset_bottom = static_cast<uint16_t>(safe_area.bottom_pixels);
    info.safe_inset_left = static_cast<uint16_t>(safe_area.left_pixels);
    info.max_surface_buffers = graphics_limits::kMaxSurfaceBuffers;
    info.max_text_bytes = graphics_limits::kMaxTextBytes;
    info.max_scene_bytes = graphics_limits::kMaxSceneBytes;
    // max_raster_bytes is filled by the Runtime, which knows whether the
    // raster kernels are bound to this session.
    return info;
}

DeviceResult<void> GraphicsService::Submit(const uint8_t* bytes, uint32_t length, const TextureAccess& textures) const {
    return StatusResult(implementation_.Submit(bytes, length, textures));
}

DeviceResult<micropixel_font_info_t> GraphicsService::LoadFont(const FontResourceView& resource) const {
    micropixel_font_info_t info{};
    const int32_t status = implementation_.LoadFont(resource, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_font_info_t>{info}
                                          : Fail<micropixel_font_info_t>(status);
}

DeviceResult<void> GraphicsService::ReleaseFont(micropixel_font_handle_t font_handle) const {
    return StatusResult(implementation_.ReleaseFont(font_handle));
}

DeviceResult<micropixel_text_metrics_t> GraphicsService::MeasureText(micropixel_font_handle_t font_handle,
                                                                     const char* text, uint32_t text_length) const {
    micropixel_text_metrics_t metrics{};
    const int32_t status = implementation_.MeasureText(font_handle, text, text_length, metrics);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_text_metrics_t>{metrics}
                                          : Fail<micropixel_text_metrics_t>(status);
}

DeviceResult<void> GraphicsService::DrawText(const TextTarget& target, int32_t x, int32_t y, uint32_t rgb888,
                                             micropixel_font_handle_t font_handle, const char* text,
                                             uint32_t text_length) const {
    const int32_t status = implementation_.DrawText(target, x, y, rgb888, font_handle, text, text_length);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<void>{} : Fail<void>(status);
}

DeviceResult<void> GraphicsService::CopyOpaqueBlocks(const PixelTarget& target, const OpaqueCopyBlock* blocks,
                                                     uint32_t count) const {
    return StatusResult(implementation_.CopyOpaqueBlocks(target, blocks, count));
}

DeviceResult<void> GraphicsService::ScaleBitmap(const BitmapView& source, const BitmapView& destination) const {
    return StatusResult(implementation_.ScaleBitmap(source, destination));
}

DeviceResult<void> GraphicsService::ShowLaunchBitmap(const BitmapView& bitmap) const {
    return StatusResult(implementation_.ShowLaunchBitmap(bitmap));
}

void GraphicsService::DismissLaunchBitmap() const { implementation_.DismissLaunchBitmap(); }

void GraphicsService::ReleaseGuestResources() const { implementation_.ReleaseGuestResources(); }

DeviceResult<DirectSurfaceInfo> GraphicsService::CreateDirectSurface(const DirectSurfaceConfig& config,
                                                                     const DirectSurfaceReleaseSink& sink) const {
    DirectSurfaceInfo info{};
    const int32_t status = implementation_.CreateDirectSurface(config, sink, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<DirectSurfaceInfo>{info} : Fail<DirectSurfaceInfo>(status);
}

DeviceResult<void> GraphicsService::PresentDirectSurface(const DirectSurfacePresentation& presentation) const {
    return StatusResult(implementation_.PresentDirectSurface(presentation));
}

void GraphicsService::SuspendDirectSurface() const { implementation_.SuspendDirectSurface(); }

void GraphicsService::ResumeDirectSurface() const { implementation_.ResumeDirectSurface(); }

DeviceResult<void> GraphicsService::DestroyDirectSurface() const {
    return StatusResult(implementation_.DestroyDirectSurface());
}

DeviceResult<micropixel_input_info_t> InputService::GetInfo() const {
    micropixel_input_info_t info{};
    int32_t status = implementation_.GetInfo(info);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_input_info_t>(status);
    }
    return info;
}

void InputService::BindTouchSink(TouchSink sink, void* context) const { implementation_.BindTouchSink(sink, context); }

void InputService::UnbindTouchSink(void* context) const { implementation_.UnbindTouchSink(context); }

bool InputService::InjectTouch(const TouchSample& sample) const { return implementation_.InjectTouch(sample); }

void InputService::BindKeySink(KeySink sink, void* context) const { implementation_.BindKeySink(sink, context); }

void InputService::UnbindKeySink(void* context) const { implementation_.UnbindKeySink(context); }

bool InputService::InjectKey(const KeySample& sample) const { return implementation_.InjectKey(sample); }

void InputService::SetActivitySink(InputActivitySink sink, void* context) const {
    implementation_.SetActivitySink(sink, context);
}

DeviceResult<micropixel_audio_info_t> AudioService::GetInfo() const {
    micropixel_audio_info_t info{};
    int32_t status = implementation_.GetInfo(info);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_audio_info_t>(status);
    }
    return info;
}

DeviceResult<void> AudioService::PlayTone(const micropixel_audio_tone_t& tone) const {
    return StatusResult(implementation_.PlayTone(tone));
}

DeviceResult<PcmStreamHandle> AudioService::StartPcm(const PcmSource& source, uint32_t token,
                                                     uint16_t volume_per_mille) const {
    PcmStreamHandle handle = 0U;
    const int32_t status = implementation_.StartPcm(source, token, volume_per_mille, handle);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<PcmStreamHandle>{handle} : Fail<PcmStreamHandle>(status);
}

DeviceResult<void> AudioService::PausePcm(PcmStreamHandle handle) const {
    return StatusResult(implementation_.PausePcm(handle));
}

DeviceResult<void> AudioService::ResumePcm(PcmStreamHandle handle) const {
    return StatusResult(implementation_.ResumePcm(handle));
}

DeviceResult<void> AudioService::SetPcmVolume(PcmStreamHandle handle, uint16_t volume_per_mille) const {
    return StatusResult(implementation_.SetPcmVolume(handle, volume_per_mille));
}

DeviceResult<void> AudioService::StopPcm(PcmStreamHandle handle) const {
    return StatusResult(implementation_.StopPcm(handle));
}

void AudioService::BindPcmCompletionSink(PcmCompletionSink sink, void* context) const {
    implementation_.BindPcmCompletionSink(sink, context);
}

void AudioService::UnbindPcmCompletionSink(void* context) const { implementation_.UnbindPcmCompletionSink(context); }

DeviceResult<void> AudioService::StopAll() const { return StatusResult(implementation_.StopAll()); }

DeviceResult<void> AudioService::SuspendAll() const { return StatusResult(implementation_.SuspendAll()); }

DeviceResult<void> AudioService::ResumeAll() const { return StatusResult(implementation_.ResumeAll()); }

DeviceResult<uint32_t> RandomService::U32() const {
    uint32_t value = 0U;
    int32_t status = implementation_.GetU32(value);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<uint32_t>(status);
    }
    return value;
}

}  // namespace micropixel::device
