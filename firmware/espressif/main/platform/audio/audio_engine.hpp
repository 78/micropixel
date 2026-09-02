#ifndef MICROPIXEL_PLATFORM_AUDIO_AUDIO_ENGINE_HPP
#define MICROPIXEL_PLATFORM_AUDIO_AUDIO_ENGINE_HPP

#include <cstdint>

#include "device/contracts/audio.hpp"
#include "esp_err.h"

namespace micropixel::platform::audio {

class AudioOutputPeripheral;
class AudioPowerController;

// Fixed-capacity Host audio engine shared by boards. A firmware image owns one
// instance and injects only its board-specific AudioOutputPeripheral.
class AudioEngine : public device::Audio {
   public:
    [[nodiscard]] esp_err_t Initialize(AudioOutputPeripheral& output, uint32_t sample_rate,
                                       AudioPowerController* power_controller = nullptr);
    virtual void SetMasterVolumePercent(uint8_t percent);
    // Board-owned switches may mute the Host mix independently of the saved
    // master-volume setting. This is intentionally outside the Guest contract.
    void SetHardwareMuted(bool muted);

    [[nodiscard]] int32_t GetInfo(micropixel_audio_info_t& info) override;
    [[nodiscard]] int32_t PlayTone(const micropixel_audio_tone_t& tone) override;
    [[nodiscard]] int32_t StartPcm(const device::PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                                   device::PcmStreamHandle& handle_out) override;
    [[nodiscard]] int32_t PausePcm(device::PcmStreamHandle handle) override;
    [[nodiscard]] int32_t ResumePcm(device::PcmStreamHandle handle) override;
    [[nodiscard]] int32_t SetPcmVolume(device::PcmStreamHandle handle, uint16_t volume_per_mille) override;
    [[nodiscard]] int32_t StopPcm(device::PcmStreamHandle handle) override;
    void BindPcmCompletionSink(device::PcmCompletionSink sink, void* context) override;
    void UnbindPcmCompletionSink(void* context) override;
    [[nodiscard]] int32_t StopAll() override;
    // GuestContext uses these as the App foreground boundary. ResumeAll keeps
    // output ready even while silent; SuspendAll enables the idle timeout.
    [[nodiscard]] int32_t SuspendAll() override;
    [[nodiscard]] int32_t ResumeAll() override;
};

}  // namespace micropixel::platform::audio

#endif
