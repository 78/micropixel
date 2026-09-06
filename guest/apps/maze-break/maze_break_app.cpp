#include "apps/maze-break/maze_break_app.hpp"

#include <stdint.h>

#include "apps/maze-break/game/renderer.hpp"
#include "apps/maze-break/game/world.hpp"
#include "apps/maze-break/gfx/font.hpp"
#include "apps/maze-break/gfx/palette.hpp"
#include "apps/maze-break/gfx/sprites.hpp"
#include "apps/maze-break/gfx/textures.hpp"
#include "apps/maze-break/input/motion_controls.hpp"
#include "apps/maze-break/input/touch_controls.hpp"
#include "apps/maze-break/maze_break_audio.hpp"
#include "apps/maze-break/rc_math.hpp"
#include "sdk/micropixel.hpp"

namespace maze_break {
namespace {

constexpr uint32_t kBufferCount = 2U;
constexpr float kTiltTurnRate = 2.6F;  // rad/s at full roll deflection
constexpr uint64_t kMaxFrameDtUs = 50'000U;
constexpr uint64_t kRecalibrateHoldUs = 1'500'000U;
constexpr uint32_t kStatsWindowFrames = 120U;
// Benchmark runs a fixed simulation step so the autopilot path is identical
// on every board regardless of frame rate.
constexpr float kBenchmarkDt = 1.0F / 40.0F;
constexpr uint64_t kBenchmarkDtUs = 25'000U;

using Line = micropixel::FixedString<224U>;

bool HasLaunchFlag(const micropixel::LaunchArguments& args, const char* name) {
    for (uint32_t index = 0U; index < args.count(); ++index) {
        const char* arg = args.Get(index);
        if (arg == nullptr) {
            continue;
        }
        uint32_t k = 0U;
        while (name[k] != '\0' && arg[k] == name[k]) {
            ++k;
        }
        if (name[k] == '\0' && (arg[k] == '\0' || arg[k] == '=')) {
            return true;
        }
    }
    return false;
}

// Panels wider than 480 px render at half resolution: the Host raster kernels
// then write a quarter of the pixels and the PPA enlarges the frame.
constexpr uint32_t kUpscaleThresholdWidth = 480U;

[[nodiscard]] uint32_t UpscaleFor(uint32_t panel_width, uint32_t panel_height) {
    return panel_width > kUpscaleThresholdWidth && panel_width % 2U == 0U && panel_height % 2U == 0U ? 2U : 1U;
}

// Virtual stick ring and knob. Touch coordinates are panel pixels; the view
// may be `upscale` times smaller.
bool DrawStickOverlay(micropixel::RasterDrawList& list, const game::Renderer& renderer,
                      const input::TouchControls::Overlay& overlay, int upscale) {
    if (!overlay.stick_active) {
        return true;
    }
    const gfx::ViewConfig& view = renderer.view();
    const uint16_t ring = gfx::PaletteRgb565(gfx::Index(gfx::kWhite, 9));
    const uint16_t knob = gfx::PaletteRgb565(gfx::Index(gfx::kCyan, 12));
    const int ring_radius = 30 * view.hud_scale;
    const int knob_radius = 8 * view.hud_scale;
    const int origin_x = overlay.stick_origin_x / upscale;
    const int origin_y = overlay.stick_origin_y / upscale;
    bool ok = renderer.DrawCircle(list, origin_x, origin_y, ring_radius, ring, false);
    int dx = (overlay.stick_x - overlay.stick_origin_x) / upscale;
    int dy = (overlay.stick_y - overlay.stick_origin_y) / upscale;
    const float len = math::Sqrt(static_cast<float>(dx * dx + dy * dy));
    if (len > static_cast<float>(ring_radius)) {
        dx = static_cast<int>(dx * ring_radius / len);
        dy = static_cast<int>(dy * ring_radius / len);
    }
    return renderer.DrawCircle(list, origin_x + dx, origin_y + dy, knob_radius, knob, true) && ok;
}

// Scripted controls for --benchmark: walk forward while sweeping the view and
// firing on a fixed cadence. Walls stop the player, so the sweep keeps the
// camera moving through different parts of the level.
game::Controls AutopilotControls(uint32_t frame) {
    game::Controls controls{};
    const float t = static_cast<float>(frame) * kBenchmarkDt;
    controls.forward = 1.0F;
    controls.strafe = math::Sin(t * 0.7F) * 0.6F;
    controls.turn = 0.35F * kBenchmarkDt + math::Sin(t * 0.23F) * 0.02F;
    controls.fire = (frame % 32U) == 0U;
    return controls;
}

struct Options {
    bool benchmark{};
    bool bgm{true};
    bool mute{};
    bool motion{true};
    bool perf{};
};

Options ParseOptions(const micropixel::LaunchArguments& args) {
    Options options{};
    options.benchmark = HasLaunchFlag(args, "--benchmark");
    options.bgm = !HasLaunchFlag(args, "--no-bgm");
    // Benchmarks measure graphics; keep the room quiet unless --sound is given.
    options.mute = HasLaunchFlag(args, "--mute") || (options.benchmark && !HasLaunchFlag(args, "--sound"));
    options.motion = !HasLaunchFlag(args, "--no-motion");
    options.perf = options.benchmark || HasLaunchFlag(args, "--perf");
    return options;
}

// The world and renderer are several KiB of arrays; they live in static
// storage rather than on the 16 KiB Guest stack. Both are trivially
// destructible, so no exit-time teardown is needed.
game::World gWorld;
game::Renderer gRenderer;

struct FrameStats {
    uint64_t window_start_us{};
    uint32_t frames{};
    uint64_t render_us{};
    uint64_t render_max_us{};
    uint64_t present_us{};
    uint64_t wait_us{};
    uint64_t frame_max_us{};
};

class MazeBreakApp final {
   public:
    int Run();

   private:
    // Returns false when the Host asked the App to stop.
    bool HandleEvent(const micropixel::Event& event);
    bool DrainEvents();
    bool WaitForFreeBuffer(uint32_t& index);
    game::Controls GatherControls(uint64_t now_us, float dt);
    void PumpSounds();
    void RequestStart();
    bool DrawInstructions(micropixel::RasterDrawList& list);
    void PublishStats(uint64_t now_us);
    void LogStats(uint64_t elapsed_us);

    micropixel::Application app_{};
    Options options_{};
    micropixel::DirectSurface surface_{};
    micropixel::SurfaceRaster raster_{};
    gfx::ViewConfig view_{};
    uint32_t upscale_{1U};
    game::Renderer& renderer_{gRenderer};
    game::World& world_{gWorld};
    input::TouchControls touch_{};
    input::MotionControls motion_{};
    bool motion_mode_{};
    bool recalibrate_armed_{true};
    GameAudio audio_{};
    game::HudStats hud_{};
    FrameStats stats_{};
    uint32_t frame_index_{};
    uint64_t last_frame_us_{};
    bool resumed_{};
    bool started_{};
    bool calibrating_{};
    bool start_touch_down_{};
    uint32_t start_touch_id_{};
    bool start_key_down_{};
    uint64_t calibration_started_us_{};
};

bool MazeBreakApp::HandleEvent(const micropixel::Event& event) {
    switch (event.type()) {
        case micropixel::EventType::kStop:
            return false;
        case micropixel::EventType::kResume:
            // The Host stopped audio and returned every buffer while we were
            // paused; the frame clock restarts so dt does not jump.
            resumed_ = true;
            return true;
        case micropixel::EventType::kTouch:
            if (!started_) {
                const auto& touch = *event.touch();
                if (!calibrating_ && touch.phase() == micropixel::TouchPhase::kDown) {
                    start_touch_down_ = true;
                    start_touch_id_ = touch.id();
                } else if (start_touch_down_ && touch.id() == start_touch_id_ &&
                           (touch.phase() == micropixel::TouchPhase::kUp ||
                            touch.phase() == micropixel::TouchPhase::kCancel)) {
                    start_touch_down_ = false;
                    if (touch.phase() == micropixel::TouchPhase::kUp) {
                        RequestStart();
                    }
                }
                return true;
            }
            touch_.OnTouch(*event.touch());
            return true;
        case micropixel::EventType::kKey:
            if (!started_) {
                if (event.key()->code() == micropixel::KeyCode::kConfirm) {
                    if (!calibrating_ && event.key()->phase() == micropixel::KeyPhase::kDown) {
                        start_key_down_ = true;
                    } else if (event.key()->phase() == micropixel::KeyPhase::kUp) {
                        const bool pressed = start_key_down_;
                        start_key_down_ = false;
                        if (pressed) {
                            RequestStart();
                        }
                    }
                }
                return true;
            }
            touch_.OnKey(*event.key());
            return true;
        case micropixel::EventType::kAudioPlayback:
            audio_.OnPlaybackEvent(event);
            return true;
        default:
            // kSurfaceReleased is consumed by the SDK before we see it; the
            // surface's Busy()/AcquireFree() state is already updated.
            return true;
    }
}

void MazeBreakApp::RequestStart() {
    if (calibrating_) {
        return;
    }
    calibrating_ = true;
    calibration_started_us_ = app_.clock().Now().microseconds();
    if (motion_mode_) {
        motion_.Recalibrate();
    }
}

bool MazeBreakApp::DrawInstructions(micropixel::RasterDrawList& list) {
    // A 240x220 diagram scales with the buffer, including half-resolution panels.
    const int unit = (view_.width < view_.height ? view_.width : view_.height);
    const int origin_x = (view_.width - unit) / 2;
    const int origin_y = (view_.height - unit * 220 / 240) / 2;
    const int text_scale = unit >= 440 ? 2 : 1;
    const uint16_t white = gfx::PaletteRgb565(gfx::Index(gfx::kWhite, 14));
    const uint16_t cyan = gfx::PaletteRgb565(gfx::Index(gfx::kCyan, 13));
    const uint16_t orange = gfx::PaletteRgb565(gfx::Index(gfx::kOrange, 13));
    const auto px = [&](int x) { return origin_x + x * unit / 240; };
    const auto py = [&](int y) { return origin_y + y * unit / 240; };
    // Render the initial world without advancing simulation; the translucent
    // tutorial sits over the exact view the player will enter.
    bool ok = renderer_.Render(list, world_, game::HudStats{.visible = false});
    ok =
        list.FillRect(micropixel::Rect{0, 0, view_.width, view_.height}, micropixel::Color::Rgb(12, 18, 28), 90U) && ok;
    const auto rect = [&](int x, int y, int w, int h, uint16_t color) {
        ok = list.FillRect(micropixel::Rect{px(x), py(y), (w * unit / 240 > 0 ? w * unit / 240 : 1),
                                            (h * unit / 240 > 0 ? h * unit / 240 : 1)},
                           micropixel::Color::FromRgb565(color)) &&
             ok;
    };
    const auto circle = [&](int x, int y, int radius, uint16_t color, bool filled) {
        ok = renderer_.DrawCircle(list, px(x), py(y), radius * unit / 240, color, filled) && ok;
    };
    const auto text = [&](int x, int y, const char* label, uint16_t color) {
        ok = renderer_.DrawText(list, px(x) - gfx::TextWidth(label, text_scale) / 2, py(y), label, color, text_scale) &&
             ok;
    };
    // Pixel-art arrows keep the diagram in the same visual language as the game.
    const auto arrow = [&](int x, int y, int dx, int dy, uint16_t color) {
        for (int i = 0; i < 13; ++i) {
            rect(x + dx * i, y + dy * i, 2, 2, color);
        }
        for (int i = 0; i < 5; ++i) {
            rect(x + dx * (12 - i) + dy * i, y + dy * (12 - i) + dx * i, 2, 2, color);
            rect(x + dx * (12 - i) - dy * i, y + dy * (12 - i) - dx * i, 2, 2, color);
        }
    };

    text(120, 0, "MAZE BREAK", white);
    // Side view: a person looks at an upright screen, held in front of the face.
    circle(72, 32, 9, white, false);
    rect(79, 30, 5, 3, white);  // nose points towards the screen
    rect(69, 42, 4, 24, white);
    rect(73, 52, 24, 3, white);  // arm and hand supporting the device
    rect(95, 48, 4, 7, white);
    rect(103, 23, 6, 35, cyan);
    rect(104, 26, 2, 28, white);
    for (int x = 87; x < 102; x += 5) {
        rect(x, 33, 2, 1, cyan);  // eye line
    }
    arrow(116, 43, 0, -1, cyan);
    text(171, 29, motion_mode_ ? "HOLD UPRIGHT" : "GET READY", white);
    text(171, 44, motion_mode_ ? "FACE SCREEN" : "TOUCH MODE", cyan);
    text(120, 73, motion_mode_ ? "TILT TO MOVE / SWING TO AIM" : "DRAG RIGHT SIDE TO LOOK", white);

    // The coloured panels cover the actual left and right touch halves.
    const int region_top = py(91);
    const int region_height = py(183) - region_top;
    ok = list.FillRect(micropixel::Rect{0, region_top, view_.width / 2, region_height},
                       micropixel::Color::Rgb(15, 48, 61), 125U) &&
         ok;
    ok = list.FillRect(micropixel::Rect{view_.width / 2, region_top, view_.width - view_.width / 2, region_height},
                       micropixel::Color::Rgb(63, 34, 24), 125U) &&
         ok;
    rect(119, 91, 2, 92, white);
    text(60, 97, "MOVE", cyan);
    text(180, 97, "FIRE", orange);
    circle(60, 138, 17, cyan, false);
    circle(60, 138, 6, cyan, true);
    arrow(60, 117, 0, -1, cyan);
    arrow(60, 159, 0, 1, cyan);
    arrow(39, 138, -1, 0, cyan);
    arrow(81, 138, 1, 0, cyan);
    // A crosshair inside a large fire pad, visually distinct from the move stick.
    circle(180, 137, 23, orange, false);
    circle(180, 137, 10, orange, false);
    rect(179, 120, 2, 10, orange);
    rect(179, 145, 2, 10, orange);
    rect(163, 136, 10, 2, orange);
    rect(188, 136, 10, 2, orange);
    circle(180, 137, 2, white, true);
    text(60, 174, "DRAG LEFT", white);
    text(180, 174, "TAP / HOLD", white);
    text(120, 191, motion_mode_ ? "FUNCTION 1.5S: RECENTER" : "FUNCTION: FIRE", white);
    rect(12, 207, 216, 13, cyan);
    text(120, 210, calibrating_ ? "HOLD STILL..." : "TAP ANYWHERE TO START",
         gfx::PaletteRgb565(gfx::Index(gfx::kGray, 1)));
    return ok;
}

bool MazeBreakApp::DrainEvents() {
    micropixel::Event event;
    while (app_.PollEvent(event)) {
        if (!HandleEvent(event)) {
            return false;
        }
    }
    return true;
}

bool MazeBreakApp::WaitForFreeBuffer(uint32_t& index) {
    while (!surface_.AcquireFree(index)) {
        const micropixel::Event event = app_.WaitEvent();
        if (!HandleEvent(event)) {
            return false;
        }
    }
    return true;
}

game::Controls MazeBreakApp::GatherControls(uint64_t now_us, float dt) {
    if (options_.benchmark) {
        return AutopilotControls(frame_index_);
    }
    game::Controls controls = touch_.Consume(now_us);
    if (motion_mode_) {
        motion_.Poll();
        const input::MotionControls::Sample motion = motion_.Consume();
        controls.forward = math::Clamp(controls.forward + motion.forward, -1.0F, 1.0F);
        controls.turn += motion.turn_rate * kTiltTurnRate * dt + motion.yaw_delta;
        // Holding the function key re-centres the neutral orientation once
        // per hold; the key also fires, which is harmless.
        if (touch_.KeyHeldFor(now_us, kRecalibrateHoldUs)) {
            if (recalibrate_armed_) {
                recalibrate_armed_ = false;
                motion_.Recalibrate();
            }
        } else {
            recalibrate_armed_ = true;
        }
    }
    return controls;
}

void MazeBreakApp::PumpSounds() {
    audio::SoundEvent sounds[game::World::kMaxPendingSounds];
    const int count = world_.TakeSounds(sounds, game::World::kMaxPendingSounds);
    for (int i = 0; i < count; ++i) {
        audio_.Play(sounds[i]);
    }
}

void MazeBreakApp::LogStats(uint64_t elapsed_us) {
    Line msg;
    msg.Append(options_.benchmark ? "maze-break-bench: frames=" : "maze-break: frames=");
    msg.AppendUint(stats_.frames);
    msg.Append(" elapsed_ms=");
    msg.AppendUint(elapsed_us / 1000U);
    msg.Append(" fps_x100=");
    msg.AppendUint(elapsed_us == 0U ? 0U : stats_.frames * 100000000ULL / elapsed_us);
    msg.Append(" render_avg_us=");
    msg.AppendUint(stats_.render_us / stats_.frames);
    msg.Append(" render_max_us=");
    msg.AppendUint(stats_.render_max_us);
    msg.Append(" present_avg_us=");
    msg.AppendUint(stats_.present_us / stats_.frames);
    msg.Append(" wait_avg_us=");
    msg.AppendUint(stats_.wait_us / stats_.frames);
    msg.Append(" frame_max_us=");
    msg.AppendUint(stats_.frame_max_us);
    msg.Append(" imps=");
    msg.AppendUint(static_cast<uint32_t>(world_.kills()));
    msg.Append("/");
    msg.AppendUint(static_cast<uint32_t>(world_.total_imps()));
    msg.Append(" hp=");
    msg.AppendInt(world_.player().health);
    app_.log().Info(msg.c_str());
}

void MazeBreakApp::PublishStats(uint64_t now_us) {
    if (stats_.frames < kStatsWindowFrames) {
        return;
    }
    const uint64_t elapsed_us = now_us - stats_.window_start_us;
    hud_.fps = elapsed_us == 0U ? 0U : static_cast<uint32_t>(stats_.frames * 1000000ULL / elapsed_us);
    hud_.render_ms_x10 = static_cast<uint32_t>(stats_.render_us / stats_.frames / 100U);
    hud_.present_ms_x10 = static_cast<uint32_t>(stats_.present_us / stats_.frames / 100U);
    hud_.wait_ms_x10 = static_cast<uint32_t>(stats_.wait_us / stats_.frames / 100U);
    if (options_.perf) {
        LogStats(elapsed_us);
    }
    stats_ = FrameStats{};
    stats_.window_start_us = now_us;
}

int MazeBreakApp::Run() {
    options_ = ParseOptions(app_.launch_arguments());

    // Host-owned buffers: the App never maps a frame, so it needs no pinned
    // linear memory; every pixel comes from the Host raster kernels (Graphics 1.6). 480 px panels render 1:1; larger
    // ones at half resolution, enlarged by the Host on present.
    const micropixel::RendererInfo display = app_.renderer().info();
    upscale_ = UpscaleFor(display.physical_width(), display.physical_height());
    auto created = app_.renderer().CreateDirectSurface(kBufferCount, micropixel::DirectSurfaceBuffers::kHost, upscale_);
    if (!created.has_value()) {
        app_.log().Error("maze-break: Host-buffer DirectSurface unavailable; Graphics 1.6 required");
        return 1;
    }
    micropixel::Result<micropixel::SurfaceRaster> raster = app_.renderer().CreateSurfaceRaster();
    if (!raster.has_value()) {
        app_.log().Error("maze-break: Host raster kernels unavailable; Graphics 1.6 required");
        return 1;
    }
    raster_ = raster.value();
    surface_ = static_cast<micropixel::DirectSurface&&>(created.value());
    if (surface_.buffer_width() > static_cast<uint32_t>(gfx::kMaxViewWidth) ||
        surface_.buffer_height() > static_cast<uint32_t>(gfx::kMaxViewHeight)) {
        app_.log().Error("maze-break: panel larger than the renderer's 800x800 limit");
        return 2;
    }
    view_.width = static_cast<int>(surface_.buffer_width());
    view_.height = static_cast<int>(surface_.buffer_height());
    view_.hud_scale = view_.width >= 720 ? 3 : (view_.width >= 400 ? 2 : 1);

    gfx::BuildPalette();
    gfx::BuildTextures();
    gfx::BuildSprites();
    renderer_.Initialize(view_);
    if (!renderer_.UploadResources(raster_)) {
        app_.log().Error("maze-break: Host raster refused the texture or palette upload");
        return 2;
    }
    touch_.Initialize(static_cast<int>(surface_.width()));

    motion_mode_ = options_.motion && !options_.benchmark && motion_.Initialize(app_);
    touch_.SetMotionMode(motion_mode_);

    world_.Reset();
    if (options_.benchmark) {
        world_.SeedRng(1U);
    }

    audio_.Initialize(app_, options_.bgm, options_.mute);
    started_ = options_.benchmark;
    if (started_) {
        audio_.StartBgm();
    }
    hud_.show_perf = options_.perf;

    {
        Line msg;
        msg.Append("maze-break: ");
        msg.AppendUint(surface_.buffer_width());
        msg.Append("x");
        msg.AppendUint(surface_.buffer_height());
        msg.Append(" Direct Surface, upscale=");
        msg.AppendUint(upscale_);
        msg.Append(surface_.direct_scanout() ? ", direct scanout" : ", composited fallback");
        msg.Append(", Host buffers + raster kernels");
        msg.Append(", panel max ");
        msg.AppendUint(surface_.max_full_frame_fps());
        msg.Append(" fps");
        app_.log().Info(msg.c_str());
        app_.log().Info(motion_mode_ ? "maze-break: motion controls; tilt to move/turn, swing to aim, right half fires"
                                     : "maze-break: touch controls; left half stick, right half look/fire");
    }

    const uint64_t start_us = app_.clock().Now().microseconds();
    last_frame_us_ = start_us;
    stats_.window_start_us = start_us;

    for (;;) {
        if (!DrainEvents()) {
            break;
        }
        uint32_t index = 0U;
        const uint64_t wait_started_us = app_.clock().Now().microseconds();
        if (!WaitForFreeBuffer(index)) {
            break;
        }
        const uint64_t now_us = app_.clock().Now().microseconds();
        stats_.wait_us += now_us - wait_started_us;

        if (resumed_) {
            resumed_ = false;
            last_frame_us_ = now_us;
            if (calibrating_) {
                calibration_started_us_ = now_us;
            }
            if (started_) {
                audio_.StartBgm();
            }
            if (motion_mode_) {
                motion_.Recalibrate();
            }
        }
        uint64_t dt_us = now_us - last_frame_us_;
        last_frame_us_ = now_us;
        if (dt_us > kMaxFrameDtUs) {
            dt_us = kMaxFrameDtUs;
        }
        if (options_.benchmark) {
            dt_us = kBenchmarkDtUs;
        }
        const float dt = static_cast<float>(dt_us) * 1e-6F;

        if (!started_ && calibrating_) {
            if (motion_mode_) {
                motion_.Poll();
                // A missing sensor must not leave the start screen stuck forever.
                if (!motion_.ready() && now_us - calibration_started_us_ >= 3'000'000U) {
                    motion_mode_ = false;
                    touch_.SetMotionMode(false);
                    calibrating_ = false;
                    app_.log().Info("maze-break: calibration timed out; showing touch instructions");
                }
            }
            if (calibrating_ && (!motion_mode_ || motion_.ready())) {
                started_ = true;
                calibrating_ = false;
                audio_.StartBgm();
                stats_ = FrameStats{};
                stats_.window_start_us = now_us;
            }
        } else if (started_) {
            const game::Controls controls = GatherControls(now_us, dt);
            const game::Phase phase_before = world_.phase();
            world_.Update(dt, controls);
            if (phase_before != game::Phase::kPlaying && world_.phase() == game::Phase::kPlaying &&
                !options_.benchmark) {
                // Both win and death retries return to the frozen first-frame
                // tutorial. Require a fresh press there, so the retry finger's
                // release cannot silently confirm a new neutral orientation.
                started_ = false;
                calibrating_ = false;
                start_touch_down_ = false;
                start_key_down_ = false;
                recalibrate_armed_ = true;
                touch_ = input::TouchControls{};
                touch_.Initialize(static_cast<int>(surface_.width()));
                touch_.SetMotionMode(motion_mode_);
                audio_.StopAll();
            }
            PumpSounds();
            audio_.Advance(dt_us);
        }

        const uint64_t render_started_us = app_.clock().Now().microseconds();
        micropixel::RasterDrawList list = raster_.Begin(surface_, index);
        bool drawn = started_ ? renderer_.Render(list, world_, hud_) : DrawInstructions(list);
        if (drawn && started_ && !options_.benchmark) {
            drawn = DrawStickOverlay(list, renderer_, touch_.overlay(), static_cast<int>(upscale_));
        }
        // The Host rasterizes the records synchronously here.
        if (!drawn || !list.Finish().has_value()) {
            app_.log().Error("maze-break: Host raster rejected the frame's records");
            return 4;
        }
        const uint64_t render_done_us = app_.clock().Now().microseconds();
        if (!surface_.Present(index).has_value()) {
            app_.log().Error("maze-break: SURFACE_PRESENT rejected");
            return 3;
        }
        const uint64_t presented_us = app_.clock().Now().microseconds();

        const uint64_t render_us = render_done_us - render_started_us;
        stats_.render_us += render_us;
        if (render_us > stats_.render_max_us) {
            stats_.render_max_us = render_us;
        }
        stats_.present_us += presented_us - render_done_us;
        const uint64_t frame_us = presented_us - now_us;
        if (frame_us > stats_.frame_max_us) {
            stats_.frame_max_us = frame_us;
        }
        ++stats_.frames;
        ++frame_index_;
        PublishStats(presented_us);
    }

    audio_.StopAll();
    surface_.Reset();
    return 0;
}

}  // namespace

int MazeBreakAppMain() {
    MazeBreakApp app;
    return app.Run();
}

}  // namespace maze_break
