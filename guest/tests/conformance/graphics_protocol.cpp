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
    if (service.interface_minor != MICROPIXEL_GRAPHICS_INTERFACE_MINOR ||
        info.max_scene_bytes != MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES ||
        info.max_scene_nodes != MICROPIXEL_GRAPHICS_MAX_SCENE_NODES ||
        info.max_batch_instances != MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES ||
        info.max_layers != MICROPIXEL_GRAPHICS_MAX_LAYERS ||
        info.max_sprite_batches != MICROPIXEL_GRAPHICS_MAX_SPRITE_BATCHES || info.reserved0 != 0U) {
        return 72;
    }

    micropixel_graphics_scene_header_t invalid{};
    invalid.magic = 0x12345678U;
    invalid.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    invalid.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    invalid.kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    invalid.total_size = sizeof(invalid);
    invalid.generation = 1U;
    invalid.revision = 1U;
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&invalid),
                                  sizeof(invalid) - 1U) != MICROPIXEL_STATUS_INVALID_ARGUMENT ||
        micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&invalid),
                                  sizeof(invalid)) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 73;
    }

    struct UnknownRecord final {
        micropixel_graphics_scene_header_t header;
        micropixel_graphics_scene_record_header_t record;
    } unknown{};
    unknown.header.magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC;
    unknown.header.interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR;
    unknown.header.interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR;
    unknown.header.kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    unknown.header.total_size = sizeof(unknown);
    unknown.header.generation = 1U;
    unknown.header.revision = 1U;
    unknown.header.record_count = 1U;
    unknown.record.opcode = 0xffffU;
    unknown.record.size = sizeof(unknown.record);
    if (micropixel_service_submit(service.handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
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
    if (renderer_info.max_scene_nodes() != info.max_scene_nodes ||
        renderer_info.max_batch_instances() != info.max_batch_instances ||
        renderer_info.max_containers() != info.max_layers ||
        renderer_info.max_sprite_batches() != info.max_sprite_batches ||
        renderer_info.max_scene_bytes() != info.max_scene_bytes ||
        safe_insets.top != scale_inset(info.safe_inset_top, renderer_info.height(), info.height) ||
        safe_insets.right != scale_inset(info.safe_inset_right, renderer_info.width(), info.width) ||
        safe_insets.bottom != scale_inset(info.safe_inset_bottom, renderer_info.height(), info.height) ||
        safe_insets.left != scale_inset(info.safe_inset_left, renderer_info.width(), info.width) ||
        renderer_info.safe_area().empty()) {
        return 75;
    }

    auto texture_result = renderer.CreateStreamingTexture(micropixel::Size{2U, 2U}, micropixel::PixelFormat::kBgr888);
    if (!texture_result) {
        return 76;
    }
    micropixel::StreamingTexture texture = static_cast<micropixel::StreamingTexture&&>(texture_result.value());
    const uint8_t texture_pixels[]{0U, 0U, 255U, 0U, 255U, 0U, 255U, 0U, 0U, 255U, 255U, 255U};
    if (!texture.Update(micropixel::Rect{0, 0, 2, 2}, texture_pixels, sizeof(texture_pixels), 6U)) {
        return 77;
    }

    auto scene = renderer.CreateScene({.logical_width = renderer_info.width(),
                                       .logical_height = renderer_info.height(),
                                       .background = micropixel::Color::Black()});
    auto game = scene.CreateContainer(
        {.clip = {0, 0, static_cast<int32_t>(renderer_info.width()), static_cast<int32_t>(renderer_info.height())}});
    // Graphics 1.4: a cached-content container travels its flag in the
    // keyframe and keeps echoing it in later patches that only move it.
    auto terrain = game.CreateContainer({.clip = {0, 100, 200, 60}, .cache_content = true});
    auto ground = terrain.CreateShape({0, 40, 400, 20}, micropixel::Color::Green());
    auto snake = game.CreateSpriteBatch(4U);
    auto image = game.CreateSurfaceNode(texture, {420, 40, 56, 56}, {0, 0, 2, 2}, 192U);
    auto label = game.CreateLabel({52, 56}, "graphics_protocol: scene keyframe", micropixel::Color::White(),
                                  micropixel::SystemFont::kMedium);
    {
        auto update = scene.BeginUpdate();
        snake.SetInstance(update, 0U,
                          {.destination = {40, 140, 20, 20}, .color = micropixel::Color::Green(), .visible = true});
        snake.SetInstance(update, 1U,
                          {.destination = {64, 140, 20, 20}, .color = micropixel::Color::Green(), .visible = true});
        if (!update.Present()) {
            return 78;
        }
    }

    {
        auto update = scene.BeginUpdate();
        snake.SetInstance(update, 0U,
                          {.destination = {88, 140, 20, 20}, .color = micropixel::Color::Green(), .visible = true});
        game.SetTranslation(update, {2, 0});
        // Translation-only patch of the cached container: FLAGS is not in the
        // property mask, the flag value is echoed unchanged.
        terrain.SetTranslation(update, {-8, 0});
        label.SetText(update, "graphics_protocol: retained patch");
        if (!update.Present()) {
            return 79;
        }
    }

    {
        // Toggling the hint is a FLAGS-only patch; content and geometry stay.
        auto update = scene.BeginUpdate();
        terrain.SetCacheContent(update, false);
        ground.SetColor(update, micropixel::Color::Rgb(200U, 40U, 40U));
        if (!update.Present()) {
            return 82;
        }
    }
    {
        auto update = scene.BeginUpdate();
        terrain.SetCacheContent(update, true);
        if (!update.Present()) {
            return 83;
        }
    }

    texture.Reset();
    micropixel::Timer redraw_guard = app.timers().After(micropixel::Duration::Milliseconds(50));
    micropixel::Event redraw_event = app.WaitEvent();
    if (redraw_event.TimerFrom(redraw_guard) == nullptr) {
        return 80;
    }
    {
        auto update = scene.BeginUpdate();
        image.SetVisible(update, false);
        label.SetText(update, "graphics_protocol: texture lifetime safe");
        if (!update.Present()) {
            return 81;
        }
    }
    app.log().Info("graphics_protocol: retained Scene keyframe/patch and resource pinning accepted");
    return 0;
}
