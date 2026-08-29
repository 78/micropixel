#ifndef MICROPIXEL_DEVICE_DEVICE_SERVICES_HPP
#define MICROPIXEL_DEVICE_DEVICE_SERVICES_HPP

#include <cstdint>
#include <expected>

#include "abi/micropixel_abi.h"
#include "device/contracts/audio.hpp"
#include "device/contracts/battery.hpp"
#include "device/contracts/devices.hpp"
#include "device/contracts/gpio.hpp"
#include "device/contracts/graphics.hpp"
#include "device/contracts/haptics.hpp"
#include "device/contracts/input.hpp"
#include "device/contracts/random.hpp"
#include "device/contracts/sensors.hpp"

namespace micropixel::device {

struct DeviceFailure final {
    int32_t status{MICROPIXEL_STATUS_INTERNAL};
};

template <typename Value>
using DeviceResult = std::expected<Value, DeviceFailure>;

class DevicesService final {
   public:
    explicit DevicesService(DeviceCatalog& implementation) : implementation_(implementation) {}

    [[nodiscard]] DeviceResult<micropixel_devices_list_response_t> List(uint16_t kind) const;
    [[nodiscard]] DeviceResult<micropixel_device_info_t> GetInfo(micropixel_device_id_t device) const;

   private:
    DeviceCatalog& implementation_;
};

class SensorsService final {
   public:
    explicit SensorsService(Sensors& implementation) : implementation_(implementation) {}

    [[nodiscard]] DeviceResult<micropixel_sensor_info_t> GetInfo(micropixel_device_id_t device) const;
    [[nodiscard]] DeviceResult<void> Start(micropixel_device_id_t device, uint32_t interval_us) const;
    [[nodiscard]] DeviceResult<SensorValues> Read(micropixel_device_id_t device) const;
    void Stop(micropixel_device_id_t device) const;

   private:
    Sensors& implementation_;
};

class GpioService final {
   public:
    explicit GpioService(Gpio& implementation) : implementation_(implementation) {}

    [[nodiscard]] DeviceResult<micropixel_gpio_info_t> GetInfo(micropixel_device_id_t device) const;
    [[nodiscard]] DeviceResult<void> Open(micropixel_device_id_t device, uint16_t mode, uint16_t pull, uint16_t edge,
                                          uint32_t initial_value, uint32_t pwm_frequency_hz, GpioEdgeSink edge_sink,
                                          void* edge_context) const;
    [[nodiscard]] DeviceResult<bool> Read(micropixel_device_id_t device) const;
    [[nodiscard]] DeviceResult<void> Write(micropixel_device_id_t device, bool value) const;
    [[nodiscard]] DeviceResult<void> SetPwmDuty(micropixel_device_id_t device, uint16_t duty_per_mille) const;
    void SuspendEvents() const;
    [[nodiscard]] DeviceResult<void> ResumeEvents() const;
    void Close(micropixel_device_id_t device) const;

   private:
    Gpio& implementation_;
};

class HapticsService final {
   public:
    explicit HapticsService(Haptics& implementation) : implementation_(implementation) {}

    [[nodiscard]] DeviceResult<micropixel_haptics_info_t> GetInfo(micropixel_device_id_t device) const;
    [[nodiscard]] DeviceResult<void> Play(micropixel_device_id_t device, uint16_t strength_per_mille,
                                          uint32_t duration_ms) const;
    [[nodiscard]] DeviceResult<void> Stop(micropixel_device_id_t device) const;
    void SetCompletionSink(HapticCompletionSink sink, void* context) const;

   private:
    Haptics& implementation_;
};

class PowerInfoService final {
   public:
    PowerInfoService(DeviceCatalog& devices, Battery& battery) : devices_(devices), battery_(battery) {}

    [[nodiscard]] DeviceResult<micropixel_power_info_response_t> Get(micropixel_device_id_t device);

   private:
    DeviceCatalog& devices_;
    Battery& battery_;
};

class GraphicsService final {
   public:
    explicit GraphicsService(Graphics& implementation) : implementation_(implementation) {}

    [[nodiscard]] bool Available() const { return implementation_.Available(); }
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
    [[nodiscard]] DeviceResult<void> ScaleBitmap(const BitmapView& source, const BitmapView& destination) const;
    [[nodiscard]] DeviceResult<void> ShowLaunchBitmap(const BitmapView& bitmap) const;
    void DismissLaunchBitmap() const;
    void ReleaseGuestResources() const;

   private:
    Graphics& implementation_;
};

class InputService final {
   public:
    explicit InputService(Input& implementation) : implementation_(implementation) {}

    [[nodiscard]] DeviceResult<micropixel_input_info_t> GetInfo() const;
    void BindTouchSink(TouchSink sink, void* context) const;
    void UnbindTouchSink(void* context) const;
    [[nodiscard]] bool InjectTouch(const TouchSample& sample) const;
    void BindKeySink(KeySink sink, void* context) const;
    void UnbindKeySink(void* context) const;
    [[nodiscard]] bool InjectKey(const KeySample& sample) const;
    void SetActivitySink(InputActivitySink sink, void* context) const;

   private:
    Input& implementation_;
};

class AudioService final {
   public:
    explicit AudioService(Audio& implementation) : implementation_(implementation) {}

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
    Audio& implementation_;
};

class RandomService final {
   public:
    explicit RandomService(Random& implementation) : implementation_(implementation) {}

    [[nodiscard]] DeviceResult<uint32_t> U32() const;

   private:
    Random& implementation_;
};

// Value-owned service façade assembled by FirmwareApp and injected into each
// Guest runtime session. Concrete Platform objects remain outside Runtime.
class DeviceServices final {
   public:
    DeviceServices(Graphics& graphics, Input& input, Audio& audio, Random& random, DeviceCatalog& devices,
                   Sensors& sensors, Gpio& gpio, Haptics& haptics, Battery& battery)
        : graphics_(graphics),
          input_(input),
          audio_(audio),
          random_(random),
          devices_(devices),
          sensors_(sensors),
          gpio_(gpio),
          haptics_(haptics),
          power_info_(devices, battery) {}
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
    [[nodiscard]] DevicesService& devices() {  // NOLINT(readability-identifier-naming)
        return devices_;
    }
    [[nodiscard]] SensorsService& sensors() {  // NOLINT(readability-identifier-naming)
        return sensors_;
    }
    [[nodiscard]] GpioService& gpio() {  // NOLINT(readability-identifier-naming)
        return gpio_;
    }
    [[nodiscard]] HapticsService& haptics() {  // NOLINT(readability-identifier-naming)
        return haptics_;
    }
    [[nodiscard]] PowerInfoService& power_info() {  // NOLINT(readability-identifier-naming)
        return power_info_;
    }

   private:
    GraphicsService graphics_;
    InputService input_;
    AudioService audio_;
    RandomService random_;
    DevicesService devices_;
    SensorsService sensors_;
    GpioService gpio_;
    HapticsService haptics_;
    PowerInfoService power_info_;
};

}  // namespace micropixel::device

#endif
