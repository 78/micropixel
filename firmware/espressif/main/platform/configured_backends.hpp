#ifndef MICROPIXEL_PLATFORM_CONFIGURED_BACKENDS_HPP
#define MICROPIXEL_PLATFORM_CONFIGURED_BACKENDS_HPP

namespace micropixel::device {
class RandomBackend;
}  // namespace micropixel::device

namespace micropixel::platform {
[[nodiscard]] device::RandomBackend& ConfiguredRandomBackend();

}  // namespace micropixel::platform

#endif
