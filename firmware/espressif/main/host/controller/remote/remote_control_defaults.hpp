#ifndef MICROPIXEL_FIRMWARE_REMOTE_CONTROL_DEFAULTS_HPP
#define MICROPIXEL_FIRMWARE_REMOTE_CONTROL_DEFAULTS_HPP

#include <string_view>

namespace micropixel::firmware::remote_control {

[[nodiscard]] constexpr bool EnabledByDefault(std::string_view service_host) { return !service_host.empty(); }

}  // namespace micropixel::firmware::remote_control

#endif  // MICROPIXEL_FIRMWARE_REMOTE_CONTROL_DEFAULTS_HPP
