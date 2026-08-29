#ifndef MICROPIXEL_PLATFORM_RANDOM_SYSTEM_RANDOM_HPP
#define MICROPIXEL_PLATFORM_RANDOM_SYSTEM_RANDOM_HPP

namespace micropixel::device {
class Random;
}  // namespace micropixel::device

namespace micropixel::platform::random {
[[nodiscard]] device::Random& SystemRandom();

}  // namespace micropixel::platform::random

#endif
