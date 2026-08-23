#ifndef MICROPIXEL_DEVICE_AUDIO_HPP
#define MICROPIXEL_DEVICE_AUDIO_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

class AudioBackend {
   public:
    virtual ~AudioBackend() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_audio_info_t& info) = 0;
    [[nodiscard]] virtual int32_t PlayTone(const micropixel_audio_tone_t& tone) = 0;
    [[nodiscard]] virtual int32_t StopAll() = 0;
    [[nodiscard]] virtual int32_t SuspendAll() = 0;
    [[nodiscard]] virtual int32_t ResumeAll() = 0;
};

}  // namespace micropixel::device

#endif
