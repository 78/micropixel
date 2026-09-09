// Tomb Explorer: a third-person room-and-portal demo of the polygon path
// (Graphics 1.6 TRIANGLE/QUAD records through the SDK MeshRenderer).
//
// Touch: left half is a stick, dragging on the right half orbits the camera,
// a tap on the right half (or the function key) jumps.
// Options: --benchmark (scripted walk, fixed step, stats every 120 frames),
// --perf (stats while playing), --upscale=N (render N times smaller).

#include <stdint.h>

#include <memory>
#include <span>

#include "apps/tomb-explorer/game/character.hpp"
#include "apps/tomb-explorer/game/math.hpp"
#include "apps/tomb-explorer/game/player.hpp"
#include "apps/tomb-explorer/gfx/palette.hpp"
#include "apps/tomb-explorer/gfx/textures.hpp"
#include "apps/tomb-explorer/input/touch_controls.hpp"
#include "apps/tomb-explorer/world/level.hpp"
#include "apps/tomb-explorer/world/room_world.hpp"
#include "sdk/mesh_renderer.hpp"
#include "sdk/micropixel.hpp"

namespace tomb {
namespace {

constexpr uint32_t kBufferCount = 2U;
constexpr uint32_t kStatsWindowFrames = 120U;
constexpr uint64_t kMaxFrameDtUs = 100'000U;
constexpr uint64_t kBenchmarkDtUs = 33'333U;
constexpr uint32_t kPolygonPool = 2048U;
constexpr float kFieldOfView = 1.25F;  // ~72 degrees horizontal
constexpr uint8_t kPaletteSlot = 0U;

using Line = micropixel::FixedString<256U>;

struct Options final {
    bool benchmark{};
    bool perf{};
    uint32_t upscale{};
};

bool HasFlag(const micropixel::LaunchArguments& args, const char* name) {
    for (uint32_t index = 0U; index < args.count(); ++index) {
        const char* arg = args.Get(index);
        const char* expected = name;
        while (*arg != '\0' && *arg == *expected) {
            ++arg;
            ++expected;
        }
        if (*arg == '\0' && *expected == '\0') return true;
    }
    return false;
}

uint32_t ParseUint(const char* text, uint32_t fallback) {
    if (text == nullptr || *text == '\0') return fallback;
    uint32_t value = 0U;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9') return fallback;
        value = value * 10U + static_cast<uint32_t>(*text - '0');
    }
    return value;
}

Options ParseOptions(const micropixel::LaunchArguments& args) {
    Options options{};
    options.benchmark = HasFlag(args, "--benchmark");
    options.perf = options.benchmark || HasFlag(args, "--perf");
    options.upscale = ParseUint(args.FindValue("--upscale"), 0U);
    if (options.upscale > 4U) options.upscale = 0U;
    return options;
}

int32_t MapCoord(int32_t value, uint32_t panel, uint32_t logical) {
    if (logical == 0U || panel == logical) return value;
    return static_cast<int32_t>(static_cast<int64_t>(value) * static_cast<int64_t>(panel) / logical);
}

// Scripted route for --benchmark: a loop through every room.
constexpr float kRoute[][2] = {
    {3.5F, 4.5F},   {3.5F, 10.5F}, {3.5F, 13.5F}, {4.5F, 15.5F},  {8.5F, 15.0F},  {12.5F, 15.5F},
    {13.5F, 17.0F}, {8.5F, 15.5F}, {0.0F, 15.0F}, {-4.5F, 15.5F}, {-6.5F, 14.0F}, {-2.5F, 15.0F},
    {2.0F, 13.0F},  {3.5F, 10.5F}, {3.5F, 4.5F},  {1.0F, 1.5F},   {5.5F, 1.5F},   {3.5F, 1.5F},
};
constexpr uint32_t kRouteLength = sizeof(kRoute) / sizeof(kRoute[0]);

class TombApp final {
   public:
    explicit TombApp(micropixel::Application& app) : app_(app) {}

    int Run();

   private:
    struct FrameStats final {
        uint64_t window_start_us{};
        uint64_t render_us{};
        uint64_t render_max_us{};
        uint64_t present_us{};
        uint64_t wait_us{};
        uint64_t faces{};
        uint64_t polygons{};
        uint64_t pixels{};
        uint64_t culled{};
        uint64_t subdivided{};
        uint32_t dropped{};
        uint32_t frames{};
        uint32_t rooms{};
    };

    bool DrainEvents();
    bool HandleEvent(const micropixel::Event& event);
    bool WaitForFreeBuffer(uint32_t& index);
    game::Controls Autopilot(float dt);
    bool Render(micropixel::RasterDrawList& list);
    void DrawStickOverlay(micropixel::RasterDrawList& list);
    void LogStats(uint64_t now_us);

    micropixel::Application& app_;
    Options options_{};
    micropixel::HostSurface surface_{};
    micropixel::RasterResources raster_{};
    uint32_t upscale_{1U};
    uint32_t logical_width_{};
    uint32_t logical_height_{};
    int width_{};
    int height_{};
    uint16_t palette_[gfx::kLightLevels * 256U]{};

    micropixel::MeshRendererPool<kPolygonPool, micropixel::MeshRenderer::kMaxGroups> pool_{};
    micropixel::MeshRenderer mesh_{};
    world::RoomWorld world_{};
    game::Player player_{};
    game::Character character_{};
    input::TouchControls touch_{};

    bool resumed_{};
    bool first_frame_{true};
    uint64_t last_frame_us_{};
    uint32_t frame_index_{};
    uint32_t route_index_{};
    FrameStats stats_{};
};

bool TombApp::DrainEvents() {
    micropixel::Event event;
    while (app_.PollEvent(event)) {
        if (!HandleEvent(event)) return false;
    }
    return true;
}

bool TombApp::HandleEvent(const micropixel::Event& event) {
    switch (event.type()) {
        case micropixel::EventType::kStop:
            return false;
        case micropixel::EventType::kResume:
            resumed_ = true;
            return true;
        case micropixel::EventType::kTouch: {
            const micropixel::TouchEvent& touch = *event.touch();
            touch_.OnTouch(touch.WithPosition({MapCoord(touch.x(), surface_.width(), logical_width_),
                                               MapCoord(touch.y(), surface_.height(), logical_height_)}));
            return true;
        }
        case micropixel::EventType::kKey:
            touch_.OnKey(*event.key());
            return true;
        default:
            return true;
    }
}

bool TombApp::WaitForFreeBuffer(uint32_t& index) {
    while (!surface_.AcquireFree(index)) {
        if (!HandleEvent(app_.WaitEvent())) return false;
    }
    return true;
}

game::Controls TombApp::Autopilot(float dt) {
    (void)dt;
    game::Controls controls{};
    const micropixel::Vec3 position = player_.position();
    const float dx = kRoute[route_index_][0] - position.x;
    const float dz = kRoute[route_index_][1] - position.z;
    if (dx * dx + dz * dz < 0.35F * 0.35F) {
        route_index_ = (route_index_ + 1U) % kRouteLength;
    }
    const float heading = math::Atan2(dx, dz);
    const float relative = math::WrapAngle(heading - player_.camera_yaw());
    controls.forward = math::Cos(relative);
    controls.strafe = math::Sin(relative);
    // A gentle camera sway exercises the portal scissors from changing angles.
    controls.tilt = math::Sin(static_cast<float>(frame_index_) * 0.02F) * 0.0025F;
    controls.jump = (frame_index_ % 300U) == 150U;
    return controls;
}

void TombApp::DrawStickOverlay(micropixel::RasterDrawList& list) {
    const input::TouchControls::Overlay overlay = touch_.overlay();
    if (!overlay.stick_active) return;
    const int scale = static_cast<int>(upscale_);
    const int ring = 70 / scale;
    const int knob = 14 / scale;
    const micropixel::Color ring_color = micropixel::Color::Rgb(220U, 220U, 220U);
    const micropixel::Color knob_color = micropixel::Color::Rgb(255U, 210U, 90U);
    const int ox = overlay.origin_x / scale;
    const int oy = overlay.origin_y / scale;
    // Ring as four thin bars, knob as a filled square.
    (void)list.FillRect({ox - ring, oy - ring, ring * 2, 2}, ring_color, 140U);
    (void)list.FillRect({ox - ring, oy + ring - 2, ring * 2, 2}, ring_color, 140U);
    (void)list.FillRect({ox - ring, oy - ring, 2, ring * 2}, ring_color, 140U);
    (void)list.FillRect({ox + ring - 2, oy - ring, 2, ring * 2}, ring_color, 140U);
    int kx = overlay.x / scale;
    int ky = overlay.y / scale;
    kx = kx < ox - ring ? ox - ring : (kx > ox + ring ? ox + ring : kx);
    ky = ky < oy - ring ? oy - ring : (ky > oy + ring ? oy + ring : ky);
    (void)list.FillRect({kx - knob / 2, ky - knob / 2, knob, knob}, knob_color, 200U);
}

bool TombApp::Render(micropixel::RasterDrawList& list) {
    list.SetPalette(kPaletteSlot);
    if (first_frame_) {
        // Closed rooms cover the buffer every frame afterwards.
        if (!list.FillRect({0, 0, width_, height_}, micropixel::Color::Rgb(0U, 0U, 0U))) return false;
    }
    const float focal = micropixel::MeshCamera::FocalLength(kFieldOfView, width_);
    mesh_.Begin(player_.Camera(world_, focal));

    world::VisibleRoom visible[world::RoomWorld::kMaxVisible];
    const uint32_t count = world_.ComputeVisible(mesh_, player_.camera_room(), {0, 0, width_, height_}, visible,
                                                 world::RoomWorld::kMaxVisible);
    for (uint32_t i = 0U; i < count; ++i) {
        const world::VisibleRoom& entry = visible[i];
        const world::Room& room = world_.room(entry.room);
        micropixel::MeshSubmitOptions options{};
        options.group = entry.group;
        options.scissor = entry.scissor;
        if (!mesh_.Submit(room.mesh(), micropixel::Transform3::Identity(), options)) return false;
        if (entry.room == player_.room()) {
            const uint32_t light = static_cast<uint32_t>(room.ambient) + 110U;
            if (!character_.Submit(mesh_, player_.position(), player_.yaw(), player_.pose(),
                                   static_cast<uint8_t>(light > 255U ? 255U : light), entry.group, entry.scissor)) {
                return false;
            }
        }
    }
    if (!mesh_.Flush(list)) return false;
    if (!options_.benchmark) DrawStickOverlay(list);

    const micropixel::MeshRenderer::Stats& stats = mesh_.stats();
    stats_.faces += stats.faces;
    stats_.polygons += stats.polygons;
    stats_.pixels += stats.pixel_estimate;
    stats_.culled += stats.culled;
    stats_.subdivided += stats.subdivided;
    stats_.dropped += stats.dropped;
    stats_.rooms += count;
    return true;
}

void TombApp::LogStats(uint64_t now_us) {
    const uint64_t elapsed_us = now_us - stats_.window_start_us;
    const uint64_t frames = stats_.frames;
    Line msg;
    msg.Append(options_.benchmark ? "tomb-bench: frames=" : "tomb: frames=");
    msg.AppendUint(frames);
    msg.Append(" fps_x100=");
    msg.AppendUint(elapsed_us == 0U ? 0U : frames * 100000000ULL / elapsed_us);
    msg.Append(" render_avg_us=");
    msg.AppendUint(stats_.render_us / frames);
    msg.Append(" render_max_us=");
    msg.AppendUint(stats_.render_max_us);
    msg.Append(" present_avg_us=");
    msg.AppendUint(stats_.present_us / frames);
    msg.Append(" wait_avg_us=");
    msg.AppendUint(stats_.wait_us / frames);
    msg.Append(" rooms=");
    msg.AppendUint(stats_.rooms / frames);
    msg.Append(" faces=");
    msg.AppendUint(stats_.faces / frames);
    msg.Append(" culled=");
    msg.AppendUint(stats_.culled / frames);
    msg.Append(" polygons=");
    msg.AppendUint(stats_.polygons / frames);
    msg.Append(" subdivided=");
    msg.AppendUint(stats_.subdivided / frames);
    msg.Append(" overdraw_x100=");
    msg.AppendUint(stats_.pixels * 100ULL / frames / (static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_)));
    msg.Append(" dropped=");
    msg.AppendUint(stats_.dropped);
    msg.Append(" room=");
    msg.AppendUint(player_.room());
    app_.log().Info(msg.c_str());
}

int TombApp::Run() {
    options_ = ParseOptions(app_.launch_arguments());
    const micropixel::RendererInfo display = app_.renderer().info();
    if (!display.polygon_supported()) {
        app_.log().Error("tomb: Host has no polygon raster records (Graphics 1.6 polygon capability)");
        return 1;
    }
    logical_width_ = display.width();
    logical_height_ = display.height();
    // 480 px panels render 1:1; larger ones at half resolution unless overridden.
    upscale_ = options_.upscale != 0U ? options_.upscale : (display.physical_width() > 480U ? 2U : 1U);
    auto created = app_.renderer().CreateHostSurface(kBufferCount, upscale_);
    if (!created.has_value()) {
        app_.log().Error("tomb: HostSurface unavailable; Graphics 1.6 required");
        return 1;
    }
    auto raster = app_.renderer().CreateRasterResources();
    if (!raster.has_value()) {
        app_.log().Error("tomb: Host raster kernels unavailable");
        return 1;
    }
    raster_ = raster.value();
    surface_ = static_cast<micropixel::HostSurface&&>(created.value());
    width_ = static_cast<int>(surface_.buffer_width());
    height_ = static_cast<int>(surface_.buffer_height());

    gfx::BuildLitPalette(palette_);
    if (!raster_.UploadLitPalette(kPaletteSlot, gfx::kLightLevels, palette_).has_value()) {
        app_.log().Error("tomb: palette upload rejected");
        return 2;
    }
    for (uint8_t slot = 0U; slot < gfx::kTextureCount; ++slot) {
        if (!raster_
                 .UploadTexture(slot, gfx::kTextureSize, gfx::kTextureSize, micropixel::RasterLayout::kRowMajor,
                                gfx::TextureTexels(slot))
                 .has_value()) {
            app_.log().Error("tomb: texture upload rejected");
            return 2;
        }
    }

    micropixel::MeshRendererConfig config{};
    config.width = width_;
    config.height = height_;
    config.lighting.levels = gfx::kLightLevels;
    config.lighting.minimum = 1U;
    config.lighting.full_distance = 5.0F;
    config.lighting.dark_distance = 40.0F;
    config.far = 48.0F;
    config.subdivide_depth_ratio = 1.6F;
    config.subdivide_min_pixels = width_ / 4;
    config.subdivide_levels = 2U;
    if (!mesh_.Initialize(config, pool_.storage(), pool_.groups())) {
        app_.log().Error("tomb: MeshRenderer initialise failed");
        return 2;
    }
    world_.Initialize(world::TombLevel());
    player_.Reset(world_);
    touch_.Initialize(static_cast<int>(surface_.width()));

    {
        Line msg;
        msg.Append("tomb: ");
        msg.AppendUint(surface_.buffer_width());
        msg.Append("x");
        msg.AppendUint(surface_.buffer_height());
        msg.Append(" Direct Surface, upscale=");
        msg.AppendUint(upscale_);
        msg.Append(surface_.direct_scanout() ? ", direct scanout" : ", composited fallback");
        msg.Append(", rooms=");
        msg.AppendUint(world_.level().rooms.size());
        msg.Append(options_.benchmark ? ", benchmark route" : ", touch: left stick, right drag orbits, tap jumps");
        app_.log().Info(msg.c_str());
    }

    const uint64_t start_us = app_.clock().Now().microseconds();
    last_frame_us_ = start_us;
    stats_.window_start_us = start_us;
    for (;;) {
        if (!DrainEvents()) break;
        uint32_t index = 0U;
        const uint64_t wait_started_us = app_.clock().Now().microseconds();
        if (!WaitForFreeBuffer(index)) break;
        const uint64_t now_us = app_.clock().Now().microseconds();
        stats_.wait_us += now_us - wait_started_us;
        if (resumed_) {
            resumed_ = false;
            last_frame_us_ = now_us;
            first_frame_ = true;
        }
        uint64_t dt_us = now_us - last_frame_us_;
        last_frame_us_ = now_us;
        if (dt_us > kMaxFrameDtUs) dt_us = kMaxFrameDtUs;
        if (options_.benchmark) dt_us = kBenchmarkDtUs;
        const float dt = static_cast<float>(dt_us) * 1e-6F;

        const game::Controls controls = options_.benchmark ? Autopilot(dt) : touch_.Consume();
        player_.Update(world_, controls, dt);

        const uint64_t render_started_us = app_.clock().Now().microseconds();
        bool drawn = false;
        auto rendered = surface_.Update(index, [&](micropixel::RasterDrawList& list) { drawn = Render(list); });
        if (!drawn || !rendered.has_value()) {
            app_.log().Error("tomb: Host raster rejected the frame's records");
            return 4;
        }
        first_frame_ = false;
        const uint64_t render_done_us = app_.clock().Now().microseconds();
        if (!surface_.Present(index).has_value()) {
            app_.log().Error("tomb: SURFACE_PRESENT rejected");
            return 3;
        }
        const uint64_t presented_us = app_.clock().Now().microseconds();

        const uint64_t render_us = render_done_us - render_started_us;
        stats_.render_us += render_us;
        if (render_us > stats_.render_max_us) stats_.render_max_us = render_us;
        stats_.present_us += presented_us - render_done_us;
        ++stats_.frames;
        ++frame_index_;
        if (stats_.frames >= kStatsWindowFrames) {
            if (options_.perf) LogStats(presented_us);
            stats_ = FrameStats{};
            stats_.window_start_us = presented_us;
        }
    }
    surface_.Reset();
    return 0;
}

}  // namespace
}  // namespace tomb

int main() {
    micropixel::Application app;
    // Level-independent state (polygon pool, palette, character) is far larger
    // than the WAMR call stack budget; keep it on the heap.
    auto tomb = std::make_unique<tomb::TombApp>(app);
    return tomb->Run();
}
