#ifndef MICROPIXEL_PLATFORM_AUDIO_VOLUME_CURVE_HPP
#define MICROPIXEL_PLATFORM_AUDIO_VOLUME_CURVE_HPP

#include <cstdint>

namespace micropixel::platform::audio {

inline constexpr uint32_t kVolumeControlScale = 10000U;

// Rounded gain for a -30 dB to 0 dB logarithmic volume law. Index zero is
// true mute; every ten slider points above it add approximately 3 dB.
inline constexpr uint16_t kVolumeDbOutput[] = {
    0U,    327U,  339U,  351U,  363U,  376U,  389U,  403U,  417U,  432U,  447U,   462U,  479U,  495U,  513U,
    531U,  550U,  569U,  589U,  610U,  631U,  653U,  676U,  700U,  724U,  750U,   776U,  804U,  832U,  861U,
    891U,  923U,  955U,  989U,  1023U, 1059U, 1096U, 1135U, 1175U, 1216U, 1259U,  1303U, 1349U, 1396U, 1445U,
    1496U, 1549U, 1603U, 1660U, 1718U, 1778U, 1841U, 1905U, 1972U, 2042U, 2113U,  2188U, 2265U, 2344U, 2427U,
    2512U, 2600U, 2692U, 2786U, 2884U, 2985U, 3090U, 3199U, 3311U, 3428U, 3548U,  3673U, 3802U, 3936U, 4074U,
    4217U, 4365U, 4519U, 4677U, 4842U, 5012U, 5188U, 5370U, 5559U, 5754U, 5957U,  6166U, 6383U, 6607U, 6839U,
    7079U, 7328U, 7586U, 7852U, 8128U, 8414U, 8710U, 9016U, 9333U, 9661U, 10000U,
};

constexpr uint16_t VolumeOutputPerTenThousand(uint8_t percent) {
    return kVolumeDbOutput[percent <= 100U ? percent : 100U];
}

constexpr int32_t ScaleOutputSample(int32_t sample, uint16_t master_volume) {
    const uint32_t clamped_master = master_volume <= kVolumeControlScale ? master_volume : kVolumeControlScale;
    const int64_t scaled = static_cast<int64_t>(sample) * clamped_master / kVolumeControlScale;
    return scaled > 32767 ? 32767 : (scaled < -32768 ? -32768 : static_cast<int32_t>(scaled));
}

}  // namespace micropixel::platform::audio

#endif
