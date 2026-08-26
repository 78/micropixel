#ifndef MICROPIXEL_DEVICE_TEXT_HPP
#define MICROPIXEL_DEVICE_TEXT_HPP

#include <cstdint>

namespace micropixel::device {

[[nodiscard]] bool IsValidUtf8(const uint8_t* text, uint32_t length);

}  // namespace micropixel::device

#endif
