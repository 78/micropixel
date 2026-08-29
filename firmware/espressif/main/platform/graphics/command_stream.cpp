#include "platform/graphics/command_stream.hpp"

#include <cstdint>
#include <cstring>

#include "abi/micropixel_abi.h"
#include "device/text.hpp"
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
    if (pixel_format == MICROPIXEL_PIXEL_FORMAT_BGR888) {
        return 3U;
    }
    return pixel_format == MICROPIXEL_PIXEL_FORMAT_BGRA8888 ? 4U : 0U;
}

bool ValidBitmapStorage(const device::BitmapView& bitmap) {
    const uint32_t bytes_per_pixel = BitmapBytesPerPixel(bitmap.pixel_format);
    if (bitmap.data == nullptr || bytes_per_pixel == 0U || bitmap.width == 0U || bitmap.height == 0U ||
        bitmap.width > UINT32_MAX / bytes_per_pixel) {
        return false;
    }
    const uint32_t row_bytes = bitmap.width * bytes_per_pixel;
    return bitmap.stride >= row_bytes && bitmap.stride % bytes_per_pixel == 0U &&
           static_cast<uint64_t>(bitmap.stride) * bitmap.height <= bitmap.size;
}

template <typename Command>
bool ValidBitmapCommand(const Command& command, int32_t logical_width, int32_t logical_height,
                        device::BitmapResolver resolver, void* resolver_context) {
    device::BitmapView bitmap{};
    return command.texture != 0U && command.x >= 0 && command.y >= 0 && command.source_x >= 0 &&
           command.source_y >= 0 && command.width > 0 && command.height > 0 && command.source_width > 0 &&
           command.source_height > 0 && static_cast<int64_t>(command.x) + command.width <= logical_width &&
           static_cast<int64_t>(command.y) + command.height <= logical_height && resolver != nullptr &&
           resolver(resolver_context, command.texture, bitmap) && ValidBitmapStorage(bitmap) &&
           static_cast<int64_t>(command.source_x) + command.source_width <= bitmap.width &&
           static_cast<int64_t>(command.source_y) + command.source_height <= bitmap.height;
}

}  // namespace

bool IsTextOpcode(uint16_t opcode) {
    return opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT || opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXT_CENTERED;
}

int32_t ValidateCommandStream(const uint8_t* bytes, uint32_t length, int32_t logical_width, int32_t logical_height,
                              device::BitmapResolver resolver, void* resolver_context,
                              device::FontValidator font_validator, void* font_context, uint32_t max_commands) {
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
    micropixel_graphics_push_state_command_t first_surface{};
    for (uint32_t index = 0U; index < header.command_count; ++index) {
        micropixel_graphics_record_header_t record{};
        if (!ReadStruct(bytes, length, offset, record) || record.size < sizeof(record) || (record.size & 3U) != 0U ||
            record.size > length - offset) {
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        }

        if (record.opcode == MICROPIXEL_GRAPHICS_OP_PUSH_STATE) {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
            micropixel_graphics_push_state_command_t command{};
            if (inside_surface || record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                command.clip_x < 0 || command.clip_y < 0 || command.width <= 0 || command.height <= 0 ||
                static_cast<int64_t>(command.clip_x) + command.width > logical_width ||
                static_cast<int64_t>(command.clip_y) + command.height > logical_height || command.translate_x < -32 ||
                command.translate_x > 32 || command.translate_y < -32 || command.translate_y > 32 ||
                (command.flags & ~MICROPIXEL_GRAPHICS_STATE_RETAINED_TRANSLATION_ACTIVE) != 0U ||
                (((command.flags & MICROPIXEL_GRAPHICS_STATE_RETAINED_TRANSLATION_ACTIVE) != 0U) &&
                 (command.clip_x + command.translate_x < 0 || command.clip_y + command.translate_y < 0 ||
                  static_cast<int64_t>(command.clip_x) + command.translate_x + command.width > logical_width ||
                  static_cast<int64_t>(command.clip_y) + command.translate_y + command.height > logical_height))) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (surface_seen &&
                (command.clip_x != first_surface.clip_x || command.clip_y != first_surface.clip_y ||
                 command.width != first_surface.width || command.height != first_surface.height ||
                 command.translate_x != first_surface.translate_x || command.translate_y != first_surface.translate_y ||
                 command.flags != first_surface.flags)) {
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
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_POP_STATE) {
#if CONFIG_MICROPIXEL_GRAPHICS_SURFACE_TRANSLATION
            micropixel_graphics_pop_state_command_t command{};
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
                command.y < 0 || command.y >= logical_height || font_validator == nullptr ||
                !font_validator(font_context, command.font_handle) || command.text_length == 0U ||
                command.text_length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES || !ValidRgb888(command.rgb888)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            uint32_t raw_size = sizeof(command) + command.text_length;
            uint32_t expected_size = (raw_size + 3U) & ~3U;
            if (record.size != expected_size ||
                !device::IsValidUtf8(bytes + offset + sizeof(command), command.text_length)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            for (uint32_t padding = raw_size; padding < record.size; ++padding) {
                if (bytes[offset + padding] != 0U) {
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                }
            }
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE) {
            micropixel_graphics_draw_texture_command_t command{};
            if (record.size != sizeof(command) || !ReadStruct(bytes, length, offset, command) ||
                !ValidBitmapCommand(command, logical_width, logical_height, resolver, resolver_context)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE) {
            micropixel_graphics_blend_texture_command_t command{};
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

bool CollectTextureHandles(const uint8_t* bytes, uint32_t length, micropixel_texture_handle_t* textures_out,
                           uint32_t capacity, uint32_t& count_out) {
    count_out = 0U;
    micropixel_graphics_command_header_t header{};
    if (textures_out == nullptr || !ReadStruct(bytes, length, 0U, header) || header.total_size != length) {
        return false;
    }
    uint32_t offset = sizeof(header);
    for (uint32_t index = 0U; index < header.command_count; ++index) {
        micropixel_graphics_record_header_t record{};
        if (!ReadStruct(bytes, length, offset, record) || record.size < sizeof(record) ||
            record.size > length - offset) {
            return false;
        }
        micropixel_texture_handle_t texture = 0U;
        if (record.opcode == MICROPIXEL_GRAPHICS_OP_DRAW_TEXTURE) {
            micropixel_graphics_draw_texture_command_t command{};
            if (!ReadStruct(bytes, length, offset, command)) {
                return false;
            }
            texture = command.texture;
        } else if (record.opcode == MICROPIXEL_GRAPHICS_OP_BLEND_TEXTURE) {
            micropixel_graphics_blend_texture_command_t command{};
            if (!ReadStruct(bytes, length, offset, command)) {
                return false;
            }
            texture = command.texture;
        }
        if (texture != 0U) {
            bool exists = false;
            for (uint32_t candidate = 0U; candidate < count_out; ++candidate) {
                if (textures_out[candidate] == texture) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                if (count_out >= capacity) {
                    return false;
                }
                textures_out[count_out++] = texture;
            }
        }
        offset += record.size;
    }
    return offset == length;
}

}  // namespace micropixel::platform::graphics
