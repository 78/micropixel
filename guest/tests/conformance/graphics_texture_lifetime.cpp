#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

// Accepted 2D snapshots retain resources independently of Guest ownership.
int main() {
    micropixel_service_info_t resource{};
    micropixel_service_info_t graphics{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_RESOURCE,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_RESOURCE_INTERFACE_MAJOR, MICROPIXEL_RESOURCE_INTERFACE_MINOR),
            &resource, sizeof(resource)) != MICROPIXEL_STATUS_OK ||
        micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &graphics, sizeof(graphics)) != MICROPIXEL_STATUS_OK) {
        return 1;
    }
    const auto call = [&](uint32_t method, const void* request, uint32_t size, void* response = nullptr,
                          uint32_t capacity = 0U) {
        uint32_t written = 0U;
        return micropixel_service_call(resource.service_handle, method, static_cast<const uint8_t*>(request), size,
                                       static_cast<uint8_t*>(response), capacity, &written);
    };
    // Method IDs past the last defined one are unknown to the Host.
    if (call(MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_UPDATE + 1U, nullptr, 0) != MICROPIXEL_STATUS_UNSUPPORTED) {
        return 2;
    }
    uint8_t pixels[]{0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255};
    micropixel_dynamic_texture_create_request_t create{};
    create.size = sizeof(create);
    create.width = create.height = 2;
    create.pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888;
    create.pixels = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pixels));
    create.length = sizeof(pixels);
    create.pitch = 6;
    micropixel_texture_info_t texture{};
    if (call(MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE, &create, sizeof(create), &texture, sizeof(texture)) !=
            MICROPIXEL_STATUS_OK ||
        !texture.texture_handle)
        return 3;
    struct Frame final {
        micropixel_graphics_scene_header_t header;
        micropixel_graphics_scene_texture_record_t image;
        micropixel_graphics_scene_background_record_t background;
        micropixel_graphics_scene_node_link_record_t link;
    } frame{};
    frame.header.magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC;
    frame.header.kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME;
    frame.header.total_size = sizeof(frame);
    frame.header.generation = 1U;
    frame.header.revision = 1U;
    frame.header.record_count = 3U;
    frame.header.node_count = 1U;
    frame.image.node.record = {MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE, sizeof(frame.image)};
    frame.image.node.flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE;
    frame.image.node.property_mask = MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY |
                                     MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE |
                                     MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                                     MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
    frame.image.x = 40;
    frame.image.y = 40;
    frame.image.width = 56;
    frame.image.height = 56;
    frame.image.texture_handle = texture.texture_handle;
    frame.image.source_width = 2;
    frame.image.source_height = 2;
    frame.image.opacity = 192U;
    frame.background.record = {MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND, sizeof(frame.background)};
    frame.background.property_mask = MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR;
    frame.link.record = {MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK, sizeof(frame.link)};
    if (micropixel_service_submit(graphics.service_handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&frame), sizeof(frame)) != MICROPIXEL_STATUS_OK) {
        return 4;
    }
    micropixel_handle_request_t release{};
    release.size = sizeof(release);
    release.handle = texture.texture_handle;
    if (call(MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD, &release, sizeof(release)) != MICROPIXEL_STATUS_OK) {
        return 5;
    }
    micropixel::Application app;
    const auto timer = app.timers().After(micropixel::Duration::Milliseconds(50)).value();
    if (app.WaitEvent().TimerFrom(timer) == nullptr) {
        return 6;
    }
    // Geometry/source/opacity patches must still render after the Guest releases
    // its handle: the retained Scene owns the remaining resource reference.
    frame.header.kind = MICROPIXEL_GRAPHICS_SCENE_PATCH;
    frame.header.base_revision = 1U;
    frame.header.revision = 2U;
    frame.header.record_count = 1U;
    frame.header.total_size = sizeof(frame.header) + sizeof(frame.image);
    frame.image.node.property_mask = MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY |
                                     MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
    frame.image.x = 80;
    frame.image.source_width = 1;
    frame.image.opacity = 255U;
    if (micropixel_service_submit(graphics.service_handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                  reinterpret_cast<const uint8_t*>(&frame),
                                  frame.header.total_size) != MICROPIXEL_STATUS_OK) {
        return 7;
    }
    const auto redraw = app.timers().After(micropixel::Duration::Milliseconds(50)).value();
    if (app.WaitEvent().TimerFrom(redraw) == nullptr) {
        return 8;
    }
    app.log().Info("graphics_texture_lifetime: retired IDs, node patches and resource pinning passed");
    return 0;
}
