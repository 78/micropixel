#ifndef MICROPIXEL_PLATFORM_AUDIO_VOLUME_CURVE_HPP
#define MICROPIXEL_PLATFORM_AUDIO_VOLUME_CURVE_HPP

#include <cstdint>

namespace micropixel::platform::audio {

inline constexpr uint32_t kVolumeControlScale = 10000U;

// Quadratic amplitude gain: (percent / 100)^2, represented exactly at scale 10000.
// Zero is true mute; 50% gives 25% amplitude and 80% gives 64% amplitude.
constexpr uint16_t VolumeOutputPerTenThousand(uint8_t percent) {
    const uint32_t clamped_percent = percent <= 100U ? percent : 100U;
    return static_cast<uint16_t>(clamped_percent * clamped_percent);
}

constexpr int32_t ScaleOutputSample(int32_t sample, uint16_t master_volume) {
    const uint32_t clamped_master = master_volume <= kVolumeControlScale ? master_volume : kVolumeControlScale;
    const int64_t scaled = static_cast<int64_t>(sample) * clamped_master / kVolumeControlScale;
    return scaled > 32767 ? 32767 : (scaled < -32768 ? -32768 : static_cast<int32_t>(scaled));
}

// Hardware mute is an independent Host gate: it silences the current sample
// without rewriting the persisted/user-selected master-volume value.
constexpr int32_t ApplyHostOutputGain(int32_t sample, uint16_t master_volume, bool hardware_muted) {
    return hardware_muted ? 0 : ScaleOutputSample(sample, master_volume);
}

}  // namespace micropixel::platform::audio

#endif
