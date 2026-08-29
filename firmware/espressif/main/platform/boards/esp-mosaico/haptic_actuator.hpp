#ifndef MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_HAPTIC_ACTUATOR_HPP
#define MICROPIXEL_PLATFORM_BOARDS_ESP_MOSAICO_HAPTIC_ACTUATOR_HPP

#include "platform/haptics/timed_haptics_peripheral.hpp"

namespace micropixel::platform::esp_mosaico {

[[nodiscard]] haptics::HapticActuator& ConfiguredHapticActuator();

}  // namespace micropixel::platform::esp_mosaico

#endif
