#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_PERCEPTUAL_CONTROL_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_PERCEPTUAL_CONTROL_HPP

#include <cstdint>

namespace micropixel::platform::metalio_claw4 {

constexpr uint32_t kPerceptualControlScale = 10000U;
constexpr uint32_t kBrightnessOutputFloor = 300U;
constexpr uint32_t kVolumeOutputFloor = 300U;

// Rounded per-ten-thousand values for 3% + 97% * pow(percent / 100, 2.2).
// A lookup table keeps the exact gamma curve without adding floating-point
// libm work to slider updates on the embedded target.
constexpr uint16_t kBrightnessGamma22Output[] = {
    300U,  300U,  302U,  304U,  308U,  313U,  320U,  328U,  337U,  349U,  361U,   375U,  391U,  409U,  428U,
    449U,  472U,  497U,  523U,  551U,  581U,  613U,  647U,  682U,  720U,  759U,   801U,  844U,  890U,  937U,
    986U,  1038U, 1091U, 1146U, 1204U, 1263U, 1325U, 1388U, 1454U, 1522U, 1592U,  1664U, 1739U, 1815U, 1894U,
    1974U, 2057U, 2142U, 2230U, 2319U, 2411U, 2505U, 2601U, 2700U, 2801U, 2904U,  3009U, 3116U, 3226U, 3338U,
    3453U, 3570U, 3689U, 3810U, 3934U, 4060U, 4188U, 4319U, 4452U, 4588U, 4726U,  4866U, 5009U, 5154U, 5301U,
    5451U, 5603U, 5758U, 5915U, 6075U, 6237U, 6402U, 6568U, 6738U, 6910U, 7084U,  7261U, 7440U, 7622U, 7806U,
    7993U, 8182U, 8374U, 8569U, 8766U, 8965U, 9167U, 9371U, 9578U, 9788U, 10000U,
};

// Rounded gain for a -30 dB to 0 dB logarithmic volume law. Index zero is
// true mute; every ten slider points above it add approximately 3 dB.
constexpr uint16_t kVolumeDbOutput[] = {
    0U,    327U,  339U,  351U,  363U,  376U,  389U,  403U,  417U,  432U,  447U,   462U,  479U,  495U,  513U,
    531U,  550U,  569U,  589U,  610U,  631U,  653U,  676U,  700U,  724U,  750U,   776U,  804U,  832U,  861U,
    891U,  923U,  955U,  989U,  1023U, 1059U, 1096U, 1135U, 1175U, 1216U, 1259U,  1303U, 1349U, 1396U, 1445U,
    1496U, 1549U, 1603U, 1660U, 1718U, 1778U, 1841U, 1905U, 1972U, 2042U, 2113U,  2188U, 2265U, 2344U, 2427U,
    2512U, 2600U, 2692U, 2786U, 2884U, 2985U, 3090U, 3199U, 3311U, 3428U, 3548U,  3673U, 3802U, 3936U, 4074U,
    4217U, 4365U, 4519U, 4677U, 4842U, 5012U, 5188U, 5370U, 5559U, 5754U, 5957U,  6166U, 6383U, 6607U, 6839U,
    7079U, 7328U, 7586U, 7852U, 8128U, 8414U, 8710U, 9016U, 9333U, 9661U, 10000U,
};

// UI brightness never turns the panel off: 0% is the dimmest usable hardware
// output. The physical backlight switch bypasses this mapping for true off.
constexpr uint32_t PerceptualBrightnessOutputPerTenThousand(uint8_t percent) {
    const uint8_t clamped_percent = percent <= 100U ? percent : 100U;
    return kBrightnessGamma22Output[clamped_percent];
}

// Volume 0% remains true mute. Active values follow equal dB increments.
constexpr uint32_t PerceptualVolumeOutputPerTenThousand(uint8_t percent) {
    const uint8_t clamped_percent = percent <= 100U ? percent : 100U;
    return kVolumeDbOutput[clamped_percent];
}

// Host volume is transparent at 100%. Guest assets own their source level and
// leave explicit mix headroom; Host only attenuates and provides a final safety
// clamp for coincident voices.
constexpr int32_t ScaleAudioOutputSample(int32_t sample, uint16_t master_volume) {
    const uint32_t clamped_master = master_volume <= kPerceptualControlScale ? master_volume : kPerceptualControlScale;
    const int64_t scaled = static_cast<int64_t>(sample) * clamped_master / kPerceptualControlScale;
    if (scaled > 32767) {
        return 32767;
    }
    if (scaled < -32768) {
        return -32768;
    }
    return static_cast<int32_t>(scaled);
}

}  // namespace micropixel::platform::metalio_claw4

#endif
