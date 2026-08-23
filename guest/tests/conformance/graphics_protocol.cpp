#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

int main() {
    micropixel_service_info_t service{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &service, sizeof(service)) != MICROPIXEL_STATUS_OK) {
        return 69;
    }

    micropixel_graphics_info_t info{};
    uint32_t response_size = 0U;
    if (micropixel_service_call(service.handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U,
                                reinterpret_cast<uint8_t*>(&info), sizeof(info) - 1U,
                                &response_size) != MICROPIXEL_STATUS_BUFFER_TOO_SMALL) {
        return 70;
    }
    if (micropixel_service_call(service.handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U,
                                reinterpret_cast<uint8_t*>(&info), sizeof(info),
                                &response_size) != MICROPIXEL_STATUS_OK ||
        response_size != sizeof(info) || info.size != sizeof(info)) {
        return 71;
    }
    if (service.interface_minor != MICROPIXEL_GRAPHICS_INTERFACE_MINOR) {
        return 75;
    }
    if ((info.capabilities & MICROPIXEL_GRAPHICS_CAP_MULTI_SUBMIT_FRAME) == 0U ||
        info.max_commands != MICROPIXEL_GRAPHICS_MAX_COMMANDS ||
        info.max_frame_commands < MICROPIXEL_GRAPHICS_MAX_FRAME_COMMANDS) {
        return 78;
    }

    micropixel_graphics_command_header_t invalid{};
    invalid.magic = 0x12345678U;
    invalid.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    invalid.total_size = sizeof(invalid);
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS,
                                  reinterpret_cast<const uint8_t*>(&invalid),
                                  sizeof(invalid) - 1U) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 72;
    }
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS,
                                  reinterpret_cast<const uint8_t*>(&invalid),
                                  sizeof(invalid)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 73;
    }

    struct UnknownCommand final {
        micropixel_graphics_command_header_t header;
        micropixel_graphics_record_header_t record;
    } unknown{};
    unknown.header.magic = MICROPIXEL_GRAPHICS_COMMAND_MAGIC;
    unknown.header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    unknown.header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    unknown.header.total_size = sizeof(unknown);
    unknown.header.command_count = 1U;
    unknown.record.opcode = 0xffffU;
    unknown.record.size = sizeof(unknown.record);
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS,
                                  reinterpret_cast<const uint8_t*>(&unknown),
                                  sizeof(unknown)) != MICROPIXEL_STATUS_UNSUPPORTED) {
        return 74;
    }

    struct BlendCommand final {
        micropixel_graphics_command_header_t header;
        micropixel_graphics_blend_rect_command_t command;
    } blend{};
    blend.header.magic = MICROPIXEL_GRAPHICS_COMMAND_MAGIC;
    blend.header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    blend.header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    blend.header.total_size = sizeof(blend);
    blend.header.command_count = 1U;
    blend.command.record.opcode = MICROPIXEL_GRAPHICS_OP_BLEND_RECT;
    blend.command.record.size = sizeof(blend.command);
    blend.command.x = 10;
    blend.command.y = 10;
    blend.command.width = 20;
    blend.command.height = 20;
    blend.command.rgb888 = 0x112233U;
    blend.command.opacity = 128U;
    blend.command.reserved[0] = 1U;
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_COMMANDS,
                                  reinterpret_cast<const uint8_t*>(&blend),
                                  sizeof(blend)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 77;
    }

    micropixel::Application app;
    micropixel::Graphics graphics = app.graphics();
    micropixel::GraphicsInfo graphics_info = graphics.info();
    micropixel::GraphicsFrame frame = graphics.BeginFrame();
    micropixel::CommandBuffer commands = frame.CreateCommandBuffer(graphics_info);
    commands.Clear(micropixel::Color::Black());
    commands.FillRect(micropixel::Rect{40, 40, 360, 56}, micropixel::Color::Green());
    commands.BlendRect(micropixel::Rect{40, 40, 360, 56}, micropixel::Color::Black(), 48U);
    commands.DrawText(52, 56, "graphics_protocol: blend accepted", micropixel::Color::White(), 18U);
    for (uint32_t index = 0U; index < 140U; ++index) {
        commands.FillRect(micropixel::Rect{40 + static_cast<int32_t>(index % 20U) * 8,
                                           140 + static_cast<int32_t>(index / 20U) * 8, 6, 6},
                          micropixel::Color::Green());
    }
    commands.Submit();
    frame.Commit();
    app.log().Info("graphics_protocol: malformed batches rejected; 128+ command frame committed atomically");
    return 0;
}
