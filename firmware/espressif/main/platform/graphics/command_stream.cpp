#include "platform/graphics/command_stream.hpp"

#include <cstdint>
#include <cstring>

#include "abi/micropixel_abi.h"
#include "sdkconfig.h"

namespace micropixel::platform::graphics {
namespace {

template <typename Value>
bool ReadStruct(const uint8_t* bytes, uint32_t length, uint32_t offset, Value& value) {
    if (offset > length || sizeof(Value) > length - offset) {
        return false;
    }
    std::memcpy(&value, bytes + offset, sizeof(Value));
    return true;
}

bool ValidRgb888(uint32_t color) { return (color & 0xff000000U) == 0U; }

bool ValidUtf8(const uint8_t* text, uint32_t length) {
    uint32_t index = 0U;
    while (index < length) {
        uint8_t first = text[index++];
        if (first == 0U) {
            return false;
        }
        if (first < 0x80U) {
            continue;
        }

        uint32_t remaining = 0U;
        uint32_t codepoint = 0U;
        uint32_t minimum = 0U;
        if ((first & 0xe0U) == 0xc0U) {
            remaining = 1U;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            remaining = 2U;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            remaining = 3U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (remaining > length - index) {
            return false;
        }
        for (uint32_t continuation = 0U; continuation < remaining; ++continuation) {
            uint8_t next = text[index++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

template <typename Command>
bool ValidRect(const Command& command, int32_t logical_width, int32_t logical_height) {
    if (command.x < 0 || command.y < 0 || command.width <= 0 || command.height <= 0) {
        return false;
    }
    int64_t right = static_cast<int64_t>(command.x) + command.width;
    int64_t bottom = static_cast<int64_t>(command.y) + command.height;
    return right <= logical_width && bottom <= logical_height;
}

uint32_t BitmapBytesPerPixel(uint32_t pixel_format) {
    if (pixel_format == MICROPIXEL_PIXEL_FORMAT_RGB888) {
        return 3U;
    }
    return pixel_format == MICROPIXEL_PIXEL_FORMAT_ARGB8888 ? 4U : 0U;
}

template <typename Command>
bool ValidBitmapCommand(const Command& command, int32_t logical_width, int32_t logical_height,
                        device::BitmapResolver resolver, void* resolver_context) {
    device::BitmapView bitmap{};
    return command.bitmap != 0U && command.x >= 0 && command.y >= 0 && command.source_x >= 0 && command.source_y >= 0 &&
           command.width > 0 && command.height > 0 &&
           static_cast<int64_t>(command.x) + command.width <= logical_width &&
           static_cast<int64_t>(command.y) + command.height <= logical_height && resolver != nullptr &&
           resolver(resolver_context, command.bitmap, bitmap) && bitmap.data != nullptr &&
           BitmapBytesPerPixel(bitmap.pixel_format) != 0U &&
           static_cast<int64_t>(command.source_x) + command.width <= bitmap.width &&
           static_cast<int64_t>(command.source_y) + command.height <= bitmap.height &&
           bitmap.stride == bitmap.width * BitmapBytesPerPixel(bitmap.pixel_format) &&
           bitmap.size == bitmap.stride * bitmap.height;
}

}  // namespace

bool IsTextOpcode(uint16_t opcode) {
    return opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT || opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED;
}

int32_t ValidateCommandStream(const uint8_t* bytes, uint32_t length, int32_t logical_width, int32_t logical_height,
                              device::BitmapResolver resolver, void* resolver_context, uint32_t max_commands) {
    micropixel_graphics_command_header_t header{};
    if (!ReadStruct(bytes, length, 0U, header) || header.magic != MICROPIXEL_GRAPHICS_COMMAND_MAGIC ||
        header.total_size != length || header.command_count > max_commands) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (header.interface_major != MICROPIXEL_GRAPHICS_INTERFACE_MAJOR ||
        header.interface_minor > MICROPIXEL_GRAPHICS_INTERFACE_MINOR) {
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }

    uint32_t offset = sizeof(header);
    bool inside_surface = false;
    bool surface_seen = false;
    micropixel_graphics_begin_surface_command_t first_surface{};
    for (uint32_t index = 0U; index < header.command_count; ++index) {
        micropixel_graphics_record_header_t record{};
        if (!ReadStruct(bytes, length, offset, record) || record.size < sizeof(record) || (record.size & 3U) != 0U ||
            record.size > length - offset) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }

        if (record.opcode == MICROPIXEL_GRAPHICS_OP_BEGIN_SURFACE) {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
            micropixel_graphics_begin_surface_command_t command{};
            if (inside_surface || record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                command.x < 0 || command.y < 0 || command.width <= 0 || command.height <= 0 ||
                static_cast<int64_t>(command.x) + command.width > logical_width ||
                static_cast<int64_t>(command.y) + command.height > logical_height || command.translate_x < -32 ||
                command.translate_x > 32 || command.translate_y < -32 || command.translate_y > 32 ||
                (command.flags & ~MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION_ACTIVE) != 0U ||
                (((command.flags & MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION_ACTIVE) != 0U) &&
                 (command.x + command.translate_x < 0 || command.y + command.translate_y < 0 ||
                  static_cast<int64_t>(command.x) + command.translate_x + command.width > logical_width ||
                  static_cast<int64_t>(command.y) + command.translate_y + command.height > logical_height))) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (surface_seen &&
                (command.x != first_surface.x || command.y != first_surface.y || command.width != first_surface.width ||
                 command.height != first_surface.height || command.translate_x != first_surface.translate_x ||
                 command.translate_y != first_surface.translate_y || command.flags != first_surface.flags)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (!surface_seen) {
                first_surface = command;
                surface_seen = true;
            }
            inside_surface = true;
#else
            return MICROPIXEL_STATUS_UNSUPPORTED;
#endif
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_END_SURFACE) {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
            micropixel_graphics_end_surface_command_t command{};
            if (!inside_surface || record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            inside_surface = false;
#else
            return MICROPIXEL_STATUS_UNSUPPORTED;
#endif
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_CLEAR) {
            micropixel_graphics_clear_command_t command{};
            if (record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                !ValidRgb888(command.rgb888)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_FILL_RECT) {
            micropixel_graphics_fill_rect_command_t command{};
            if (record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                !ValidRect(command, logical_width, logical_height) || !ValidRgb888(command.rgb888)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_RECT) {
            micropixel_graphics_blend_rect_command_t command{};
            if (record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                !ValidRect(command, logical_width, logical_height) || !ValidRgb888(command.rgb888) ||
                command.reserved[0] != 0U || command.reserved[1] != 0U || command.reserved[2] != 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        } else if (IsTextOpcode(record.opcode)) {
            micropixel_graphics_draw_text_command_t command{};
            if (!ReadStruct(bytes, length, offset, command) || command.x < 0 || command.x >= logical_width ||
                command.y < 0 || command.y >= logical_height || command.font_size_px < 8U ||
                command.font_size_px > 48U || command.text_length == 0U ||
                command.text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES || !ValidRgb888(command.rgb888)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            uint32_t raw_size = sizeof(command) + command.text_length;
            uint32_t expected_size = (raw_size + 3U) & ~3U;
            if (record.size != expected_size || !ValidUtf8(bytes + offset + sizeof(command), command.text_length)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            for (uint32_t padding = raw_size; padding < record.size; ++padding) {
                if (bytes[offset + padding] != 0U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
            }
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_BITMAP) {
            micropixel_graphics_draw_bitmap_command_t command{};
            if (record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                !ValidBitmapCommand(command, logical_width, logical_height, resolver, resolver_context)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_BITMAP) {
            micropixel_graphics_blend_bitmap_command_t command{};
            if (record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                !ValidBitmapCommand(command, logical_width, logical_height, resolver, resolver_context) ||
                command.reserved[0] != 0U || command.reserved[1] != 0U || command.reserved[2] != 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        } else {
            return MICROPIXEL_STATUS_UNSUPPORTED;
        }
        offset += record.size;
    }
    return offset == length && !inside_surface ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INVALID_ARGUMENT;
}

}  // namespace micropixel::platform::graphics
