#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace micropixel::platform::common::audio {

// Hardware output boundary for the shared mixer. Samples are signed stereo
// 32-bit frames with the 16-bit PCM value in the high half-word.
class AudioSink {
   public:
    virtual ~AudioSink() = default;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    [[nodiscard]] virtual esp_err_t Initialize() = 0;
    [[nodiscard]] virtual esp_err_t Start(int32_t* scratch_frames, uint32_t frame_count) = 0;
    [[nodiscard]] virtual esp_err_t Write(const int32_t* frames, uint32_t frame_count) = 0;
    [[nodiscard]] virtual esp_err_t Stop() = 0;
    virtual void Shutdown() = 0;
    [[nodiscard]] virtual const char* Name() const = 0;

   protected:
    AudioSink() = default;
};

}  // namespace micropixel::platform::common::audio
