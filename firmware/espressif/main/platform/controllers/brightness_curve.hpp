#ifndef MICROPIXEL_PLATFORM_CONTROLLERS_BRIGHTNESS_CURVE_HPP
#define MICROPIXEL_PLATFORM_CONTROLLERS_BRIGHTNESS_CURVE_HPP

#include <cstdint>

namespace micropixel::platform::controllers {

inline constexpr uint32_t kBrightnessControlScale = 10000U;
inline constexpr uint32_t kBrightnessOutputFloor = 300U;

// Rounded per-ten-thousand values for 3% + 97% * pow(percent / 100, 2.2).
// User brightness never turns a panel off: 0% is the dimmest usable output.
// Power and display-suspend paths bypass this mapping when a true off state is
// required.
inline constexpr uint16_t kBrightnessGamma22Output[] = {
    300U,  300U,  302U,  304U,  308U,  313U,  320U,  328U,  337U,  349U,  361U,   375U,  391U,  409U,  428U,
    449U,  472U,  497U,  523U,  551U,  581U,  613U,  647U,  682U,  720U,  759U,   801U,  844U,  890U,  937U,
    986U,  1038U, 1091U, 1146U, 1204U, 1263U, 1325U, 1388U, 1454U, 1522U, 1592U,  1664U, 1739U, 1815U, 1894U,
    1974U, 2057U, 2142U, 2230U, 2319U, 2411U, 2505U, 2601U, 2700U, 2801U, 2904U,  3009U, 3116U, 3226U, 3338U,
    3453U, 3570U, 3689U, 3810U, 3934U, 4060U, 4188U, 4319U, 4452U, 4588U, 4726U,  4866U, 5009U, 5154U, 5301U,
    5451U, 5603U, 5758U, 5915U, 6075U, 6237U, 6402U, 6568U, 6738U, 6910U, 7084U,  7261U, 7440U, 7622U, 7806U,
    7993U, 8182U, 8374U, 8569U, 8766U, 8965U, 9167U, 9371U, 9578U, 9788U, 10000U,
};

constexpr uint32_t BrightnessOutputPerTenThousand(uint8_t percent, uint32_t output_floor = kBrightnessOutputFloor) {
    const uint32_t clamped_floor = output_floor <= kBrightnessControlScale ? output_floor : kBrightnessControlScale;
    const uint32_t base_output = kBrightnessGamma22Output[percent <= 100U ? percent : 100U];
    constexpr uint32_t kBaseRange = kBrightnessControlScale - kBrightnessOutputFloor;
    const uint32_t configured_range = kBrightnessControlScale - clamped_floor;
    return clamped_floor + ((base_output - kBrightnessOutputFloor) * configured_range + kBaseRange / 2U) / kBaseRange;
}

// Some panel drivers expose only whole percentages. Round upward so the
// safety floor and every non-zero curve step remain visible after conversion.
constexpr uint8_t VisibleBrightnessPercent(uint8_t percent, uint32_t output_floor = kBrightnessOutputFloor) {
    return static_cast<uint8_t>((BrightnessOutputPerTenThousand(percent, output_floor) + 99U) / 100U);
}

}  // namespace micropixel::platform::controllers

#endif
