#ifndef MICROPIXEL_PLATFORM_METALIO_CLAW4_PERIPHERAL_IDS_HPP
#define MICROPIXEL_PLATFORM_METALIO_CLAW4_PERIPHERAL_IDS_HPP

#include <array>

#include "abi/micropixel_abi.h"

namespace micropixel::platform::metalio_claw4::peripheral_ids {

inline constexpr micropixel_device_id_t kDisplay = 0x00010001U;
inline constexpr micropixel_device_id_t kTouch = 0x00010002U;
inline constexpr micropixel_device_id_t kAudioOutput = 0x00010003U;
inline constexpr micropixel_device_id_t kAcceleration = 0x00020001U;
inline constexpr micropixel_device_id_t kMagneticField = 0x00020002U;
inline constexpr micropixel_device_id_t kHaptics = 0x00040001U;
inline constexpr micropixel_device_id_t kPower = 0x00050001U;
inline constexpr std::array<uint16_t, 14> kApplicationGpioLines{5U,  14U, 15U, 16U, 17U, 18U, 19U,
                                                                20U, 21U, 23U, 35U, 46U, 47U, 48U};

[[nodiscard]] constexpr micropixel_device_id_t Gpio(uint16_t line_number) { return 0x00030000U | line_number; }

}  // namespace micropixel::platform::metalio_claw4::peripheral_ids

#endif
