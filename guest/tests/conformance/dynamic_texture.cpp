#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

int main() {
    using namespace micropixel;
    Application app;
    auto renderer = app.renderer();
    micropixel_service_info_t service{};
    if (micropixel_service_open(MICROPIXEL_SERVICE_RESOURCE, MICROPIXEL_INTERFACE_VERSION(1, 2), &service,
                                sizeof(service)) != MICROPIXEL_STATUS_VERSION_MISMATCH)
        return 101;
    if (micropixel_service_open(MICROPIXEL_SERVICE_RESOURCE, MICROPIXEL_INTERFACE_VERSION(2, 0), &service,
                                sizeof(service)) != MICROPIXEL_STATUS_OK)
        return 102;
    micropixel_dynamic_texture_create_request_t invalid{};
    invalid.size = sizeof(invalid);
    invalid.width = invalid.height = 2;
    invalid.pixel_format = MICROPIXEL_PIXEL_FORMAT_BGRA8888;
    invalid.pixels = UINT32_MAX - 7;
    invalid.length = 16;
    invalid.pitch = 8;
    micropixel_texture_info_t response{};
    uint32_t response_size{};
    if (micropixel_service_call(service.service_handle, MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE,
                                reinterpret_cast<const uint8_t*>(&invalid), sizeof(invalid),
                                reinterpret_cast<uint8_t*>(&response), sizeof(response),
                                &response_size) != MICROPIXEL_STATUS_INVALID_MEMORY)
        return 103;
    uint8_t pixels[]{0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 0, 0, 0, 0};
    auto texture = app.resources().CreateDynamicTexture({2, 2}, PixelFormat::kBgra8888, pixels, 8);
    if (!texture) return 104;
    if (texture->Update({1, 1, 2, 2}, pixels, 8).error().code() != ErrorCode::kInvalidArgument) return 105;
    auto page = renderer.CreateScene(Color::Rgb(20, 24, 36)).value();
    auto preview = page.CreateSprite(*texture, {24, 96, 96, 96}, {0, 0, 2, 2}).value();
    auto title =
        page.CreateLabel({24, 24}, "Dynamic texture / shared references", Color::White(), SystemFont::kSmall).value();
    // A second page referencing the same texture; switching pages must not
    // lose the snapshot bound by the other Scene.
    auto other = renderer.CreateScene(Color::Rgb(36, 24, 20)).value();
    auto mirror = other.CreateSprite(*texture, {200, 96, 96, 96}, {0, 0, 2, 2}).value();
    (void)preview;
    (void)title;
    (void)mirror;
    const bool hold = app.launch_arguments().FindValue("--hold") != nullptr;
    for (uint32_t frame = 0; frame < 60 || hold;) {
        // References were bound once. Updating the texture must refresh every
        // Scene that shows it, including after a Present returned WouldBlock.
        pixels[0] = frame % 2 ? 255 : 0;
        pixels[2] = frame % 2 ? 0 : 255;
        if (!texture->Update({0, 0, 1, 1}, {pixels, 4}, 4)) return 106;
        auto shown = frame % 20 == 19 ? renderer.Present(other) : renderer.Present(page);
        if (shown) {
            ++frame;
            if (frame == 60) app.log().Info("dynamic_texture: atomic snapshots, shared references and pages passed");
        } else if (shown.error().code() != ErrorCode::kWouldBlock)
            return 107;
        Event event;
        if (!app.WaitEventFor(event, Duration::Milliseconds(30))) continue;
        if (event.type() == EventType::kStop) return hold ? 0 : 108;
    }
    return 0;
}
