// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdlib>
#include <cstring>

#include "runtime/display_context.cpp"
#include "runtime/graphics.cpp"
#include "sdk/application.hpp"

namespace {
micropixel_texture_load_request_t last_texture_request{};
uint32_t surface_upscale{};
uint32_t texture_calls{};
bool scene_alive{};
}  // namespace

namespace micropixel {
Application::Application() noexcept = default;
DirectSurface::~DirectSurface() = default;
}  // namespace micropixel
namespace micropixel::runtime {
[[noreturn]] void Panic(const char*, int32_t) { std::abort(); }
void RequireOk(int32_t status, const char*) { assert(status == MICROPIXEL_STATUS_OK); }
uint32_t ActiveSurfaceUpscale() { return surface_upscale; }
bool AnyLiveScene() { return scene_alive; }
int32_t OpenService(ServiceCache& cache, uint32_t id, uint16_t major, uint16_t minor) {
    cache.info.service_handle = id;
    cache.info.interface_major = major;
    cache.info.interface_minor = minor;
    return MICROPIXEL_STATUS_OK;
}
int32_t CallVoid(ServiceCache&, uint32_t, const void*, uint32_t) { return MICROPIXEL_STATUS_OK; }
int32_t CallService(ServiceCache& cache, uint32_t method, const void* request, uint32_t request_size, void* response,
                    uint32_t capacity, uint32_t& size) {
    if (cache.info.service_handle == MICROPIXEL_SERVICE_GRAPHICS && method == MICROPIXEL_GRAPHICS_METHOD_GET_INFO) {
        micropixel_graphics_info_t info{};
        info.size = sizeof(info);
        info.width = info.height = 480;
        info.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
        info.max_surface_buffers = 2;
        info.max_text_bytes = 1024;
        info.max_scene_bytes = 65536;
        assert(capacity >= sizeof(info));
        std::memcpy(response, &info, sizeof(info));
        size = sizeof(info);
        return MICROPIXEL_STATUS_OK;
    }
    assert(cache.info.service_handle == MICROPIXEL_SERVICE_RESOURCE &&
           method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_LOAD);
    assert(request_size == sizeof(last_texture_request));
    std::memcpy(&last_texture_request, request, request_size);
    ++texture_calls;
    micropixel_texture_info_t info{};
    info.size = sizeof(info);
    info.texture_handle = texture_calls;
    info.width = 320;
    info.height = 240;
    info.physical_width = info.width * last_texture_request.scale_numerator / last_texture_request.scale_denominator;
    info.physical_height = info.height * last_texture_request.scale_numerator / last_texture_request.scale_denominator;
    info.pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565;
    assert(capacity >= sizeof(info));
    std::memcpy(response, &info, sizeof(info));
    size = sizeof(info);
    return MICROPIXEL_STATUS_OK;
}
}  // namespace micropixel::runtime

class TestSurface final : public micropixel::DirectSurface {
   public:
    TestSurface() {
        handle_ = 1;
        width_ = height_ = 480;
        buffer_width_ = buffer_height_ = 240;
    }
};

int main() {
    using namespace micropixel;
    Application app;
    const auto renderer = app.renderer();
    const auto resources = app.resources();
    const AssetId asset{1};
    {
        auto native = resources.LoadTexture(asset);
        assert(native && native->width() == 320 && native->height() == 240);
        assert(last_texture_request.scale_numerator == 1 && last_texture_request.scale_denominator == 1);
        assert(renderer.ConfigureDisplay({}).error().code() == ErrorCode::kInvalidState);
    }
    // A fresh Guest instance, while retaining the fake physical display.
    runtime::display_context_loaded = false;
    runtime::display_context_configured = false;
    assert(!renderer.ConfigureDisplay({{0, 240}, DisplayScaleMode::kAspectFit}));
    assert(renderer.ConfigureDisplay({{320, 240}, DisplayScaleMode::kAspectFit}));
    assert(!renderer.ConfigureDisplay({{0, 240}, DisplayScaleMode::kAspectFit}));
    auto configured = resources.LoadTexture(asset);
    assert(configured && last_texture_request.scale_numerator == 3 && last_texture_request.scale_denominator == 2);
    const auto info = renderer.info();
    assert(info.width() == 320 && info.height() == 240 && info.physical_width() == 480 &&
           info.physical_height() == 480);
    assert(runtime::ToLogical({240, 240}) == (Point{160, 120}));
    assert(runtime::ToLogical({0, 30}) == (Point{0, -20}));
    assert(!renderer.ConfigureDisplay({}));
    TestSurface target;
    assert(target.ToBuffer(Point{160, 120}) == (Point{120, 120}));
    assert(target.ToBuffer(Point{0, 0}) == (Point{0, 30}));
    assert(target.ToLogical({120, 120}) == (Point{160, 120}));
    assert(target.ToBuffer(Rect{0, 0, 320, 240}) == (Rect{0, 30, 240, 180}));
    auto target_texture = resources.LoadTexture(asset, target.texture_scale());
    assert(target_texture && last_texture_request.scale_numerator == 3 && last_texture_request.scale_denominator == 4);
    auto native = resources.LoadTexture(asset, TextureScale::kNative);
    assert(native && last_texture_request.scale_numerator == 1 && last_texture_request.scale_denominator == 1);
    assert(!resources.LoadTexture(asset, TextureScale::kSurface));
    surface_upscale = 2;
    auto surface = resources.LoadTexture(asset, TextureScale::kSurface);
    assert(surface && last_texture_request.scale_numerator == 3 && last_texture_request.scale_denominator == 4);
    auto still_configured = resources.LoadTexture(asset);
    assert(still_configured && last_texture_request.scale_numerator == 3 &&
           last_texture_request.scale_denominator == 2);
    auto explicit_scale = resources.LoadTexture(asset, TextureLoadOptions::ForShortEdge(320, 240, 240));
    assert(explicit_scale && last_texture_request.scale_numerator == 3 && last_texture_request.scale_denominator == 4);
    auto reduced = resources.LoadTexture(asset, TextureLoadOptions::Ratio(10000, 20000));
    assert(reduced && last_texture_request.scale_numerator == 1 && last_texture_request.scale_denominator == 2);
    const auto calls = texture_calls;
    assert(!resources.LoadTexture(asset, TextureLoadOptions::Ratio(0, 1)));
    assert(!resources.LoadTexture(asset, TextureLoadOptions::Ratio(1, 0)));
    assert(!resources.LoadTexture(asset, TextureLoadOptions::Ratio(4097, 4096)));
    assert(!resources.LoadTexture(asset, static_cast<TextureScale>(255)));
    assert(texture_calls == calls);

    // An explicitly configured canvas is never replaced by a surface.
    assert(!runtime::AdoptSurfaceCanvas(240, 240));
    assert(renderer.info().width() == 320);

    // Surface-only App: reading info() first (to pick the upscale) is fine; the
    // surface then adopts its buffer size as the logical canvas.
    runtime::display_context_loaded = false;
    runtime::display_context_configured = false;
    assert(renderer.info().width() == 480);
    assert(runtime::AdoptSurfaceCanvas(240, 240));
    assert(renderer.info().width() == 240 && renderer.info().height() == 240 &&
           renderer.info().physical_width() == 480);
    assert(runtime::ToLogical({480, 240}) == (Point{240, 120}));
    assert(target.ToBuffer(Point{100, 70}) == (Point{100, 70}));
    assert(target.ToBuffer(Rect{10, 20, 30, 40}) == (Rect{10, 20, 30, 40}));
    assert(target.ToLogical({100, 70}) == (Point{100, 70}));
    // Default loads still follow the display scale (logical -> physical, 2:1);
    // surface.texture_scale() divides by the upscale and lands at 1:1.
    auto adopted = resources.LoadTexture(asset);
    assert(adopted && last_texture_request.scale_numerator == 2 && last_texture_request.scale_denominator == 1);
    auto adopted_surface = resources.LoadTexture(asset, target.texture_scale());
    assert(adopted_surface && last_texture_request.scale_numerator == 1 && last_texture_request.scale_denominator == 1);
    assert(!renderer.ConfigureDisplay({{320, 240}, DisplayScaleMode::kAspectFit}));
    // The canvas is frozen: a second surface with another upscale keeps it.
    assert(!runtime::AdoptSurfaceCanvas(480, 480));
    assert(renderer.info().width() == 240);

    // A live Scene commits the native canvas; the surface leaves it alone.
    runtime::display_context_loaded = false;
    runtime::display_context_configured = false;
    scene_alive = true;
    assert(!runtime::AdoptSurfaceCanvas(240, 240));
    assert(renderer.info().width() == 480);
    scene_alive = false;

    // A buffer that does not tile the panel cannot become the canvas.
    runtime::display_context_loaded = false;
    runtime::display_context_configured = false;
    assert(!runtime::AdoptSurfaceCanvas(200, 240));
    assert(!runtime::display_context_configured);
}
