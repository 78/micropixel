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

DeviceResult<micropixel_graphics_info_t> GraphicsService::GetInfo() const {
    micropixel_graphics_info_t info{};
    int32_t status = backend_.GetInfo(info);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_graphics_info_t>(status);
    }
    return info;
}

DeviceResult<void> GraphicsService::BeginFrame() const { return StatusResult(backend_.BeginFrame()); }

DeviceResult<void> GraphicsService::Submit(const uint8_t* bytes, uint32_t length, const TextureAccess& textures) const {
    return StatusResult(backend_.Submit(bytes, length, textures));
}

DeviceResult<void> GraphicsService::CommitFrame(const TextureAccess& textures) const {
    return StatusResult(backend_.CommitFrame(textures));
}

DeviceResult<void> GraphicsService::CancelFrame() const { return StatusResult(backend_.CancelFrame()); }

DeviceResult<micropixel_font_info_t> GraphicsService::LoadFont(const FontResourceView& resource) const {
    micropixel_font_info_t info{};
    const int32_t status = backend_.LoadFont(resource, info);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_font_info_t>{info}
                                          : Fail<micropixel_font_info_t>(status);
}

DeviceResult<void> GraphicsService::ReleaseFont(micropixel_font_handle_t font) const {
    return StatusResult(backend_.ReleaseFont(font));
}

DeviceResult<micropixel_text_metrics_t> GraphicsService::MeasureText(micropixel_font_handle_t font, const char* text,
                                                                     uint32_t text_length) const {
    micropixel_text_metrics_t metrics{};
    const int32_t status = backend_.MeasureText(font, text, text_length, metrics);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<micropixel_text_metrics_t>{metrics}
                                          : Fail<micropixel_text_metrics_t>(status);
}

DeviceResult<void> GraphicsService::BeginBitmapUpdateFrame() const {
    return StatusResult(backend_.BeginBitmapUpdateFrame());
}

DeviceResult<void> GraphicsService::UpdateBitmap(const BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                                 uint32_t height, const uint8_t* pixels, uint32_t stride) const {
    return StatusResult(backend_.UpdateBitmap(bitmap, x, y, width, height, pixels, stride));
}

DeviceResult<void> GraphicsService::CommitBitmapUpdateFrame() const {
    return StatusResult(backend_.CommitBitmapUpdateFrame());
}

DeviceResult<void> GraphicsService::ShowLaunchBitmap(const BitmapView& bitmap) const {
    return StatusResult(backend_.ShowLaunchBitmap(bitmap));
}

void GraphicsService::DismissLaunchBitmap() const { backend_.DismissLaunchBitmap(); }

void GraphicsService::ReleaseGuestResources() const { backend_.ReleaseGuestResources(); }

DeviceResult<micropixel_input_info_t> InputService::GetInfo() const {
    micropixel_input_info_t info{};
    int32_t status = backend_.GetInfo(info);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_input_info_t>(status);
    }
    return info;
}

void InputService::BindTouchSink(TouchSink sink, void* context) const { backend_.BindTouchSink(sink, context); }

void InputService::UnbindTouchSink(void* context) const { backend_.UnbindTouchSink(context); }

bool InputService::InjectTouch(const TouchSample& sample) const { return backend_.InjectTouch(sample); }

void InputService::BindKeySink(KeySink sink, void* context) const { backend_.BindKeySink(sink, context); }

void InputService::UnbindKeySink(void* context) const { backend_.UnbindKeySink(context); }

bool InputService::InjectKey(const KeySample& sample) const { return backend_.InjectKey(sample); }

void InputService::SetActivitySink(InputActivitySink sink, void* context) const {
    backend_.SetActivitySink(sink, context);
}

DeviceResult<micropixel_audio_info_t> AudioService::GetInfo() const {
    micropixel_audio_info_t info{};
    int32_t status = backend_.GetInfo(info);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<micropixel_audio_info_t>(status);
    }
    return info;
}

DeviceResult<void> AudioService::PlayTone(const micropixel_audio_tone_t& tone) const {
    return StatusResult(backend_.PlayTone(tone));
}

DeviceResult<PcmStreamHandle> AudioService::StartPcm(const PcmSource& source, uint32_t token,
                                                     uint16_t volume_per_mille) const {
    PcmStreamHandle handle = 0U;
    const int32_t status = backend_.StartPcm(source, token, volume_per_mille, handle);
    return status == MICROPIXEL_STATUS_OK ? DeviceResult<PcmStreamHandle>{handle} : Fail<PcmStreamHandle>(status);
}

DeviceResult<void> AudioService::PausePcm(PcmStreamHandle handle) const {
    return StatusResult(backend_.PausePcm(handle));
}

DeviceResult<void> AudioService::ResumePcm(PcmStreamHandle handle) const {
    return StatusResult(backend_.ResumePcm(handle));
}

DeviceResult<void> AudioService::SetPcmVolume(PcmStreamHandle handle, uint16_t volume_per_mille) const {
    return StatusResult(backend_.SetPcmVolume(handle, volume_per_mille));
}

DeviceResult<void> AudioService::StopPcm(PcmStreamHandle handle) const {
    return StatusResult(backend_.StopPcm(handle));
}

void AudioService::BindPcmCompletionSink(PcmCompletionSink sink, void* context) const {
    backend_.BindPcmCompletionSink(sink, context);
}

void AudioService::UnbindPcmCompletionSink(void* context) const { backend_.UnbindPcmCompletionSink(context); }

DeviceResult<void> AudioService::StopAll() const { return StatusResult(backend_.StopAll()); }

DeviceResult<void> AudioService::SuspendAll() const { return StatusResult(backend_.SuspendAll()); }

DeviceResult<void> AudioService::ResumeAll() const { return StatusResult(backend_.ResumeAll()); }

DeviceResult<uint32_t> RandomService::U32() const {
    uint32_t value = 0U;
    int32_t status = backend_.GetU32(value);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<uint32_t>(status);
    }
    return value;
}

}  // namespace micropixel::device
