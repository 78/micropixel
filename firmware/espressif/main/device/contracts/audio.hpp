#ifndef MICROPIXEL_DEVICE_AUDIO_HPP
#define MICROPIXEL_DEVICE_AUDIO_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

using PcmStreamHandle = uint32_t;

struct PcmReadResult final {
    uint32_t frames{};
    bool finished{};
};

using PcmReadCallback = PcmReadResult (*)(void* context, int16_t* mono_samples, uint32_t frame_capacity);

struct PcmSource final {
    PcmReadCallback read{};
    void* context{};
};

struct PcmCompletion final {
    uint32_t token{};
    int32_t status{MICROPIXEL_STATUS_OK};
};

using PcmCompletionSink = void (*)(void* context, const PcmCompletion& completion);

class Audio {
   public:
    virtual ~Audio() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_audio_info_t& info) = 0;
    [[nodiscard]] virtual int32_t PlayTone(const micropixel_audio_tone_t& tone) = 0;
    [[nodiscard]] virtual int32_t StartPcm(const PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                                           PcmStreamHandle& handle_out) = 0;
    [[nodiscard]] virtual int32_t PausePcm(PcmStreamHandle handle) = 0;
    [[nodiscard]] virtual int32_t ResumePcm(PcmStreamHandle handle) = 0;
    [[nodiscard]] virtual int32_t SetPcmVolume(PcmStreamHandle handle, uint16_t volume_per_mille) = 0;
    [[nodiscard]] virtual int32_t StopPcm(PcmStreamHandle handle) = 0;
    virtual void BindPcmCompletionSink(PcmCompletionSink sink, void* context) = 0;
    virtual void UnbindPcmCompletionSink(void* context) = 0;
    [[nodiscard]] virtual int32_t StopAll() = 0;
    // These methods also define the owning AppSession's foreground state:
    // ResumeAll keeps output ready, while SuspendAll permits idle shutdown.
    [[nodiscard]] virtual int32_t SuspendAll() = 0;
    [[nodiscard]] virtual int32_t ResumeAll() = 0;
};

}  // namespace micropixel::device

#endif
