#ifndef MICROPIXEL_PLATFORM_GRAPHICS_COMMAND_STREAM_HPP
#define MICROPIXEL_PLATFORM_GRAPHICS_COMMAND_STREAM_HPP

#include <cstdint>

#include "device/graphics.hpp"

namespace micropixel::platform::graphics {

[[nodiscard]] bool IsTextOpcode(uint16_t opcode);

[[nodiscard]] int32_t ValidateCommandStream(const uint8_t* bytes, uint32_t length, int32_t logical_width,
                                            int32_t logical_height, device::BitmapResolver resolver,
                                            void* resolver_context,
                                            uint32_t max_commands = MICROPIXEL_GRAPHICS_MAX_COMMANDS);

}  // namespace micropixel::platform::graphics

#endif
