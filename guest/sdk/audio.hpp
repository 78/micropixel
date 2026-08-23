#ifndef MICROPIXEL_SDK_AUDIO_HPP
#define MICROPIXEL_SDK_AUDIO_HPP

#include <stdint.h>

#include "sdk/result.hpp"
#include "sdk/types.hpp"

namespace micropixel {

enum class Waveform : uint16_t {
    kSine = 1U,
    kSquare = 2U,
    kTriangle = 3U,
    kNoise = 4U,
};

struct AudioInfo final {
    uint32_t sample_rate{};
    uint16_t max_voices{};
    uint32_t supported_waveforms{};
    Duration maximum_tone_duration{};
};

struct Tone final {
    Waveform waveform{Waveform::kSine};
    uint32_t frequency_hz{440U};
    Duration duration{Duration::Milliseconds(150U)};
    uint16_t volume_per_mille{60U};
    Duration attack{Duration::Milliseconds(5U)};
    Duration release{Duration::Milliseconds(20U)};
};

// A bounded fire-and-forget synthesizer for short UI/game tones. The Host
// mixes a fixed voice pool; no Guest PCM buffer or chip-specific I2S detail is
// exposed. Long-form compressed/streaming audio can be added as an independent
// data plane when a real application requires it.
class Audio final {
   public:
    [[nodiscard]] Result<AudioInfo> info() const;
    [[nodiscard]] Result<void> Play(const Tone& tone) const;
    [[nodiscard]] Result<void> StopAll() const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Audio(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
