#ifndef MICROPIXEL_DEVICE_DEVICE_SERVICES_HPP
#define MICROPIXEL_DEVICE_DEVICE_SERVICES_HPP

#include <cstdint>
#include <expected>

#include "abi/micropixel_abi.h"
#include "device/audio.hpp"
#include "device/graphics.hpp"
#include "device/input.hpp"
#include "device/random.hpp"

namespace micropixel::device {

struct DeviceFailure final {
    int32_t status{MICROPIXEL_STATUS_INTERNAL};
};

template <typename Value>
using DeviceResult = std::expected<Value, DeviceFailure>;

class GraphicsService final {
   public:
    explicit GraphicsService(GraphicsBackend& backend) : backend_(backend) {}

    [[nodiscard]] bool Available() const { return backend_.Available(); }
    [[nodiscard]] DeviceResult<micropixel_graphics_info_t> GetInfo() const;
    [[nodiscard]] DeviceResult<void> BeginFrame() const;
    [[nodiscard]] DeviceResult<void> Submit(const uint8_t* bytes, uint32_t length, const TextureAccess& textures) const;
    [[nodiscard]] DeviceResult<void> CommitFrame(const TextureAccess& textures) const;
    [[nodiscard]] DeviceResult<void> CancelFrame() const;
    [[nodiscard]] DeviceResult<micropixel_font_info_t> LoadFont(const FontResourceView& resource) const;
    [[nodiscard]] DeviceResult<void> ReleaseFont(micropixel_font_handle_t font) const;
    [[nodiscard]] DeviceResult<micropixel_text_metrics_t> MeasureText(micropixel_font_handle_t font, const char* text,
                                                                      uint32_t text_length) const;
    [[nodiscard]] DeviceResult<void> BeginBitmapUpdateFrame() const;
    [[nodiscard]] DeviceResult<void> UpdateBitmap(const BitmapView& bitmap, uint32_t x, uint32_t y, uint32_t width,
                                                  uint32_t height, const uint8_t* pixels, uint32_t stride) const;
    [[nodiscard]] DeviceResult<void> CommitBitmapUpdateFrame() const;
    [[nodiscard]] DeviceResult<void> ShowLaunchBitmap(const BitmapView& bitmap) const;
    void DismissLaunchBitmap() const;
    void ReleaseGuestResources() const;

   private:
    GraphicsBackend& backend_;
};

class InputService final {
   public:
    explicit InputService(InputBackend& backend) : backend_(backend) {}

    [[nodiscard]] DeviceResult<micropixel_input_info_t> GetInfo() const;
    void BindTouchSink(TouchSink sink, void* context) const;
    void UnbindTouchSink(void* context) const;
    [[nodiscard]] bool InjectTouch(const TouchSample& sample) const;
    void BindKeySink(KeySink sink, void* context) const;
    void UnbindKeySink(void* context) const;
    [[nodiscard]] bool InjectKey(const KeySample& sample) const;
    void SetActivitySink(InputActivitySink sink, void* context) const;

   private:
    InputBackend& backend_;
};

class AudioService final {
   public:
    explicit AudioService(AudioBackend& backend) : backend_(backend) {}

    [[nodiscard]] DeviceResult<micropixel_audio_info_t> GetInfo() const;
    [[nodiscard]] DeviceResult<void> PlayTone(const micropixel_audio_tone_t& tone) const;
    [[nodiscard]] DeviceResult<PcmStreamHandle> StartPcm(const PcmSource& source, uint32_t token,
                                                         uint16_t volume_per_mille) const;
    [[nodiscard]] DeviceResult<void> PausePcm(PcmStreamHandle handle) const;
    [[nodiscard]] DeviceResult<void> ResumePcm(PcmStreamHandle handle) const;
    [[nodiscard]] DeviceResult<void> SetPcmVolume(PcmStreamHandle handle, uint16_t volume_per_mille) const;
    [[nodiscard]] DeviceResult<void> StopPcm(PcmStreamHandle handle) const;
    void BindPcmCompletionSink(PcmCompletionSink sink, void* context) const;
    void UnbindPcmCompletionSink(void* context) const;
    [[nodiscard]] DeviceResult<void> StopAll() const;
    [[nodiscard]] DeviceResult<void> SuspendAll() const;
    [[nodiscard]] DeviceResult<void> ResumeAll() const;

   private:
    AudioBackend& backend_;
};

class RandomService final {
   public:
    explicit RandomService(RandomBackend& backend) : backend_(backend) {}

    [[nodiscard]] DeviceResult<uint32_t> U32() const;

   private:
    RandomBackend& backend_;
};

// Value-owned service façade assembled by FirmwareApp and injected into each
// Guest runtime session. Platform handles remain private to the board backend.
class DeviceServices final {
   public:
    DeviceServices(GraphicsBackend& graphics, InputBackend& input, AudioBackend& audio, RandomBackend& random)
        : graphics_(graphics), input_(input), audio_(audio), random_(random) {}
    DeviceServices(const DeviceServices&) = delete;
    DeviceServices& operator=(const DeviceServices&) = delete;

    [[nodiscard]] GraphicsService& graphics() {  // NOLINT(readability-identifier-naming)
        return graphics_;
    }
    [[nodiscard]] InputService& input() {  // NOLINT(readability-identifier-naming)
        return input_;
    }
    [[nodiscard]] AudioService& audio() {  // NOLINT(readability-identifier-naming)
        return audio_;
    }
    [[nodiscard]] RandomService& random() {  // NOLINT(readability-identifier-naming)
        return random_;
    }

   private:
    GraphicsService graphics_;
    InputService input_;
    AudioService audio_;
    RandomService random_;
};

}  // namespace micropixel::device

#endif
