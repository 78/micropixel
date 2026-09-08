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
    if (micropixel_service_call(service.service_handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U,
                                reinterpret_cast<uint8_t*>(&info), sizeof(info) - 1U,
                                &response_size) != MICROPIXEL_STATUS_BUFFER_TOO_SMALL) {
        return 70;
    }
    if (micropixel_service_call(service.service_handle, MICROPIXEL_GRAPHICS_METHOD_GET_INFO, nullptr, 0U,
                                reinterpret_cast<uint8_t*>(&info), sizeof(info),
                                &response_size) != MICROPIXEL_STATUS_OK ||
        response_size != sizeof(info) || info.size != sizeof(info)) {
        return 71;
    }
    if (service.interface_minor != MICROPIXEL_GRAPHICS_INTERFACE_MINOR || info.reserved0 != 0U) {
        return 72;
    }

    micropixel_graphics_scene_header_t invalid{};
    invalid.magic = 0x12345678U;
    invalid.kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    invalid.total_size = sizeof(invalid);
    invalid.generation = 1U;
    invalid.revision = 1U;
    if (micropixel_service_submit(service.service_handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&invalid),
                                  sizeof(invalid) - 1U) != MICROPIXEL_STATUS_INVALID_ARGUMENT ||
        micropixel_service_submit(service.service_handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&invalid),
                                  sizeof(invalid)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 73;
    }

    struct UnknownRecord final {
        micropixel_graphics_scene_header_t header;
        micropixel_graphics_scene_record_header_t record;
    } unknown{};
    unknown.header.magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC;
    unknown.header.kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    unknown.header.total_size = sizeof(unknown);
    unknown.header.generation = 1U;
    unknown.header.revision = 1U;
    unknown.header.record_count = 1U;
    unknown.record.opcode = 0xffffU;
    unknown.record.size = sizeof(unknown.record);
    if (micropixel_service_submit(service.service_handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&unknown),
                                  sizeof(unknown)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 74;
    }

    micropixel::Application app;
    micropixel::Renderer renderer = app.renderer();
    const auto renderer_info = renderer.info();
    const auto safe_insets = renderer_info.safe_area_insets();
    const auto scale_inset = [](uint32_t value, uint32_t logical_extent, uint32_t physical_extent) {
        return (static_cast<uint64_t>(value) * logical_extent + physical_extent - 1U) / physical_extent;
    };
    if (safe_insets.top != scale_inset(info.safe_inset_top, renderer_info.height(), info.height) ||
        safe_insets.right != scale_inset(info.safe_inset_right, renderer_info.width(), info.width) ||
        safe_insets.bottom != scale_inset(info.safe_inset_bottom, renderer_info.height(), info.height) ||
        safe_insets.left != scale_inset(info.safe_inset_left, renderer_info.width(), info.width) ||
        renderer_info.safe_area().empty()) {
        return 75;
    }

    auto scene = renderer
                     .CreateScene({.logical_width = renderer_info.width(),
                                   .logical_height = renderer_info.height(),
                                   .background = micropixel::Color::Black()})
                     .value();
    auto game = scene
                    .CreateContainer({.clip = {0, 0, static_cast<int32_t>(renderer_info.width()),
                                               static_cast<int32_t>(renderer_info.height())}})
                    .value();
    // Graphics 1.4: a cached-content container travels its flag in the
    // keyframe and keeps echoing it in later patches that only move it.
    auto terrain = game.CreateContainer({.clip = {0, 100, 200, 60}, .cache_content = true}).value();
    auto ground = terrain.CreateShape({0, 40, 400, 20}, micropixel::Color::Green()).value();
    auto snake = game.CreateSpriteBatch(4U).value();
    auto label = game.CreateLabel({52, 56}, "graphics_protocol: scene keyframe", micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium)
                     .value();

    snake.SetInstanceVisible(2U, false);
    snake.SetInstanceVisible(3U, false);
    snake.SetInstance(0U, {.destination = {40, 140, 20, 20}, .color = micropixel::Color::Green(), .visible = true});
    snake.SetInstance(1U, {.destination = {64, 140, 20, 20}, .color = micropixel::Color::Green(), .visible = true});

    if (!renderer.Present(scene)) {
        return 78;
    }

    snake.SetInstance(0U, {.destination = {88, 140, 20, 20}, .color = micropixel::Color::Green(), .visible = true});
    game.SetTranslation({2, 0});
    // Translation-only patch of the cached container: FLAGS is not in the
    // property mask, the flag value is echoed unchanged.
    terrain.SetTranslation({-8, 0});
    label.SetText("graphics_protocol: retained patch");

    if (!renderer.Present(scene)) {
        return 79;
    }

    // Toggling the hint is a FLAGS-only patch; content and geometry stay.

    terrain.SetCacheContent(false);
    ground.SetColor(micropixel::Color::Rgb(200U, 40U, 40U));

    if (!renderer.Present(scene)) {
        return 82;
    }
    terrain.SetCacheContent(true);
    if (!renderer.Present(scene)) {
        return 83;
    }

    // Graphics 1.7: grow an existing scene to 1024 total instances, then
    // patch its final slot. Node count no longer consumes instance capacity.
    auto expanded = game.CreateSpriteBatch(1020U).value();

    for (uint16_t index = 0U; index < 1019U; ++index) {
        expanded.SetInstanceVisible(index, false);
    }
    expanded.SetInstance(1019U, {.destination = {100, 200, 12, 12}, .color = micropixel::Color::Green()});

    if (!renderer.Present(scene)) {
        return 84;
    }
    expanded.SetInstanceVisible(1019U, false);
    if (!renderer.Present(scene)) {
        return 85;
    }

    app.log().Info("graphics_protocol: 1024 instances and growth/patch accepted");
    return 0;
}
