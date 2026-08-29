#ifndef MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_HAPTIC_ACTUATOR_HPP
#define MICROPIXEL_PLATFORM_BOARDS_METALIO_CLAW4_HAPTIC_ACTUATOR_HPP

#include "platform/haptics/timed_haptics_peripheral.hpp"

namespace micropixel::platform::metalio_claw4 {

[[nodiscard]] haptics::HapticActuator& ConfiguredHapticActuator();

}  // namespace micropixel::platform::metalio_claw4

#endif
