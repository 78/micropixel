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

DeviceResult<micropixel_devices_list_response_t> DevicesService::List(uint16_t kind) const {
    if (kind > MICROPIXEL_DEVICE_KIND_NETWORK) {
        return Fail<micropixel_devices_list_response_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint32_t count = implementation_.Count();
    if (count > MICROPIXEL_MAX_DEVICES) {
        return Fail<micropixel_devices_list_response_t>(MICROPIXEL_STATUS_INTERNAL);
    }
    micropixel_devices_list_response_t response{};
    response.size = sizeof(response);
    response.generation = implementation_.Generation();
    for (uint32_t index = 0U; index < count; ++index) {
        micropixel_device_info_t info{};
        const int32_t status = implementation_.GetByIndex(index, info);
        if (status != MICROPIXEL_STATUS_OK || info.device == 0U || info.kind == MICROPIXEL_DEVICE_KIND_ANY ||
            info.kind > MICROPIXEL_DEVICE_KIND_NETWORK) {
            const int32_t failure =
                status == MICROPIXEL_STATUS_OK ? static_cast<int32_t>(MICROPIXEL_STATUS_INTERNAL) : status;
            return Fail<micropixel_devices_list_response_t>(failure);
        }
        if (kind == MICROPIXEL_DEVICE_KIND_ANY || info.kind == kind) {
            response.devices[response.count++] = info.device;
        }
    }
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

DeviceResult<micropixel_power_info_response_t> PowerInfoService::Get(micropixel_device_id_t device) {
    micropixel_device_info_t device_info{};
    const int32_t device_status = devices_.GetById(device, device_info);
    if (device_status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_power_info_response_t>(device_status);
    }
    if (device_info.kind != MICROPIXEL_DEVICE_KIND_POWER) {
        return Fail<micropixel_power_info_response_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }

    const BatterySnapshot snapshot = battery_.Snapshot();
    micropixel_power_info_response_t response{};
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
    return info;
}

DeviceResult<void> GraphicsService::BeginFrame() const { return StatusResult(implementation_.BeginFrame()); }

DeviceResult<void> GraphicsService::Submit(const uint8_t* bytes, uint32_t length, const TextureAccess& textures) const {
    return StatusResult(implementation_.Submit(bytes, length, textures));
}

DeviceResult<void> GraphicsService::CommitFrame(const TextureAccess& textures) const {
    return StatusResult(implementation_.CommitFrame(textures));
}

DeviceResult<void> GraphicsService::CancelFrame() const { return StatusResult(implementation_.CancelFrame()); }

DeviceResult<micropixel_font_info_t> GraphicsService::LoadFont(const FontResourceView& resource) const {
    micropixel_font_info_t info{};
    const int32_t status = implementation_.LoadFont(resource, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_font_info_t>{info}
                                          : Fail<micropixel_font_info_t>(status);
}

DeviceResult<void> GraphicsService::ReleaseFont(micropixel_font_handle_t font) const {
    return StatusResult(implementation_.ReleaseFont(font));
}

DeviceResult<micropixel_text_metrics_t> GraphicsService::MeasureText(micropixel_font_handle_t font, const char* text,
                                                                     uint32_t text_length) const {
    micropixel_text_metrics_t metrics{};
    const int32_t status = implementation_.MeasureText(font, text, text_length, metrics);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_text_metrics_t>{metrics}
                                          : Fail<micropixel_text_metrics_t>(status);
}

DeviceResult<void> GraphicsService::BeginBitmapUpdateFrame() const {
    return StatusResult(implementation_.BeginBitmapUpdateFrame());
}

DeviceResult<void> GraphicsService::UpdateBitmap(const BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                                 uint32_t height, const uint8_t* pixels, uint32_t stride) const {
    return StatusResult(implementation_.UpdateBitmap(bitmap, x, y, width, height, pixels, stride));
}

DeviceResult<void> GraphicsService::CommitBitmapUpdateFrame() const {
    return StatusResult(implementation_.CommitBitmapUpdateFrame());
}

DeviceResult<void> GraphicsService::ScaleBitmap(const BitmapView& source, const BitmapView& destination) const {
    return StatusResult(implementation_.ScaleBitmap(source, destination));
}

DeviceResult<void> GraphicsService::ShowLaunchBitmap(const BitmapView& bitmap) const {
    return StatusResult(implementation_.ShowLaunchBitmap(bitmap));
}

void GraphicsService::DismissLaunchBitmap() const { implementation_.DismissLaunchBitmap(); }

void GraphicsService::ReleaseGuestResources() const { implementation_.ReleaseGuestResources(); }

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
