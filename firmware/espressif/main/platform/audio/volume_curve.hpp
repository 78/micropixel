#ifndef MICROPIXEL_PLATFORM_AUDIO_VOLUME_CURVE_HPP
#define MICROPIXEL_PLATFORM_AUDIO_VOLUME_CURVE_HPP

#include <cstdint>

namespace micropixel::platform::audio {

inline constexpr uint32_t kVolumeControlScale = 10000U;

// Rounded logarithmic gain from -54 dB at 1% to 0 dB at 100%.
// The first active setting matches 10% of the former -60 dB curve.
// Zero is true mute. Gain = round(10000 * 10^(-54 * (100 - percent) / 99 / 20)).
inline constexpr uint16_t kVolumeDbOutput[] = {
    0U,    20U,   21U,   23U,   24U,   26U,   27U,   29U,   31U,   33U,   35U,    37U,   40U,   42U,   45U,
    48U,   51U,   54U,   58U,   62U,   66U,   70U,   75U,   79U,   85U,   90U,    96U,   102U,  109U,  116U,
    123U,  131U,  140U,  149U,  158U,  169U,  180U,  191U,  204U,  217U,  231U,   246U,  262U,  279U,  297U,
    316U,  337U,  359U,  382U,  407U,  433U,  461U,  491U,  523U,  556U,  593U,   631U,  672U,  715U,  762U,
    811U,  864U,  920U,  979U,  1043U, 1110U, 1182U, 1259U, 1341U, 1427U, 1520U,  1618U, 1723U, 1835U, 1954U,
    2081U, 2215U, 2359U, 2512U, 2675U, 2848U, 3033U, 3229U, 3438U, 3661U, 3899U,  4151U, 4420U, 4707U, 5012U,
    5337U, 5683U, 6051U, 6443U, 6861U, 7305U, 7779U, 8283U, 8820U, 9391U, 10000U,
};

constexpr uint16_t VolumeOutputPerTenThousand(uint8_t percent) {
    return kVolumeDbOutput[percent <= 100U ? percent : 100U];
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
