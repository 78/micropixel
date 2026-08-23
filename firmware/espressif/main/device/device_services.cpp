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

DeviceResult<void> GraphicsService::BeginFrame() const {
    return StatusResult(backend_.BeginFrame());
}

DeviceResult<void> GraphicsService::Submit(const uint8_t* bytes, uint32_t length, BitmapResolver resolver,
                                           void* resolver_context) const {
    return StatusResult(backend_.Submit(bytes, length, resolver, resolver_context));
}

DeviceResult<void> GraphicsService::CommitFrame(BitmapResolver resolver, void* resolver_context) const {
    return StatusResult(backend_.CommitFrame(resolver, resolver_context));
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

DeviceResult<void> AudioService::StopAll() const { return StatusResult(backend_.StopAll()); }

DeviceResult<uint32_t> RandomService::U32() const {
    uint32_t value = 0U;
    int32_t status = backend_.GetU32(value);
    if (status != MICROPIXEL_STATUS_OK) {
        return Fail<uint32_t>(status);
    }
    return value;
}

}  // namespace micropixel::device
