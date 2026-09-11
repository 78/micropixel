// Gravity Balls: lit spheres (8..64, default 24) in a box that is fixed to
// the device. The accelerometer (and, when present, the gyroscope) supplies
// the inertial forces felt inside the box, so tilting, shaking and spinning
// the device moves the balls the way a real box of marbles would. A MENU
// button opens a settings panel for ball count, box depth and gravity.
//
// Rendering budget (480x480 Host buffer): the background is pure black, so a
// frame only re-clears the rectangles the previous frame on that buffer
// touched, redraws the wireframe box (about twenty 2 px FlatQuads), and adds
// one additive light pool plus one additive glow and one opaque, pre-lit
// sprite per ball. Glows are drawn before any ball so they only light the
// background. The spheres are baked once into an INDEX8 atlas at eleven
// sizes with anti-aliased rims (coverage darkens toward black), and the
// palette rows select the colour, so a ball costs one SPRITE record.
#include <algorithm>
#include <cmath>
#include <memory>

#include "physics.hpp"
#include "sdk/fixed_string.hpp"
#include "sdk/micropixel.hpp"

namespace gravity_balls {
namespace mp = micropixel;

constexpr unsigned kColors = 8;
constexpr unsigned kPaletteRows = 24;  // 8 ball rows, 8 glow rows, 8 pool rows
constexpr uint8_t kGlowRow = 8;
constexpr uint8_t kPoolRow = 16;
constexpr uint8_t kMaxShade = 252;   // ball / blob texels use 1..252
constexpr uint8_t kGridIndex = 253;  // palette entry of the dim box rulers
constexpr uint8_t kWireIndex = 254;  // palette entry of the box edges
// Ball sprite diameters authored for a 480 px buffer; larger buffers scale
// them up (at most kMaxAtlasScale) so near balls are never magnified past
// their baked anti-aliased rim.
constexpr unsigned kBaseDiameters[] = {14, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52};
constexpr unsigned kSizeCount = sizeof(kBaseDiameters) / sizeof(kBaseDiameters[0]);
constexpr unsigned kBaseBufferSize = 480;
constexpr float kMaxAtlasScale = 1.5F;
constexpr unsigned kBaseDiameterSum = 14 + 16 + 20 + 24 + 28 + 32 + 36 + 40 + 44 + 48 + 52;
constexpr unsigned kMaxDiameter = static_cast<unsigned>(52 * kMaxAtlasScale);
constexpr unsigned kBlobSize = 64;
constexpr unsigned kAtlasHeight = kMaxDiameter > kBlobSize ? kMaxDiameter : kBlobSize;
constexpr unsigned kAtlasWidth = static_cast<unsigned>(kBaseDiameterSum * kMaxAtlasScale) + kSizeCount + kBlobSize;
constexpr uint8_t kTextureSlot = 0;
constexpr uint8_t kPaletteSlot = 0;
constexpr unsigned kBufferCount = 3;
constexpr unsigned kWireLines = 22;
constexpr unsigned kMaxDirty = World::kMaxCount * 2 + 4;
constexpr unsigned kStatsWindow = 120;
constexpr unsigned kMaxStepsPerFrame = 8;
// View-space depth of the box's open front face (the glass). The camera sits
// at the origin looking down +z; the front face exactly fills the buffer.
constexpr float kNear = 10.0F;
constexpr float kBoxHalfSize = 4.4F;
// Settings menu choices: ball count, box depth (full depth in world
// units; the default is roughly a cube) and gravity as a multiple of the
// measured acceleration.
constexpr unsigned kBallOptions[] = {8, 16, 24, 32, 48, 64};
constexpr float kDepthOptions[] = {6.0F, 9.0F, 12.0F, 15.0F};
constexpr float kGravityOptions[] = {0.5F, 1.0F, 1.5F, 2.0F};
constexpr unsigned kDefaultBallOption = 2, kDefaultDepthOption = 2, kDefaultGravityOption = 1;
constexpr unsigned kSettingRows = 3;
// Sign that maps the accelerometer z axis onto "into the screen". Tilt
// established X opposite to and Y along screen axes on ESP-Mosaico; with a
// right-handed sensor reading +1 g on the axis pointing up, gravity into the
// screen is +z. Flip this if balls rest against the glass when lying flat.
constexpr float kDepthAxisSign = 1.0F;
constexpr float kMaxGravity = 40.0F;  // m/s^2, before the world scale
constexpr uint64_t kSensorTimeoutUs = 500000;

constexpr uint8_t kPalette[kColors][3] = {{255, 83, 116}, {255, 173, 60},  {255, 225, 80},  {70, 225, 160},
                                          {65, 190, 255}, {130, 110, 255}, {235, 100, 230}, {105, 235, 240}};

using Line = mp::FixedString<240>;

[[nodiscard]] uint16_t Rgb565(float r, float g, float b) {
    const auto channel = [](float v, unsigned bits) {
        const float top = static_cast<float>((1U << bits) - 1U);
        v = std::clamp(v, 0.0F, 255.0F) * top / 255.0F;
        return static_cast<unsigned>(v + 0.5F);
    };
    return static_cast<uint16_t>((channel(r, 5) << 11) | (channel(g, 6) << 5) | channel(b, 5));
}

[[nodiscard]] int Round(float v) { return static_cast<int>(v + (v >= 0 ? 0.5F : -0.5F)); }
// The restricted Guest links no libc: integer abs by hand.
[[nodiscard]] int Distance(int a, int b) { return a > b ? a - b : b - a; }

struct WireLine final {
    Vector a{}, b{};
    uint8_t index{};
};

struct BallDraw final {
    mp::Rect ball{}, glow{}, pool{};
    float depth{};
    uint8_t size{}, color{};
    bool pool_visible{};
};

class Demo final {
   public:
    int Run() {
        const auto display = app_.renderer().info();
        if (!display.raster_supported()) {
            app_.log().Error("gravity-balls: Host raster kernels required");
            return 1;
        }
        auto surface = app_.renderer().CreateHostSurface(kBufferCount, display.physical_width() > 480 ? 2 : 1);
        if (!surface) return 1;
        surface_ = std::move(*surface);
        auto resources = app_.renderer().CreateRasterResources();
        if (!resources) return 1;
        resources_ = *resources;
        width_ = static_cast<int>(surface_.buffer_width());
        height_ = static_cast<int>(surface_.buffer_height());
        touch_scale_x_ = static_cast<float>(width_) / static_cast<float>(std::max(1U, display.width()));
        touch_scale_y_ = static_cast<float>(height_) / static_cast<float>(std::max(1U, display.height()));
        polygons_ = display.polygon_supported();
        additive_ = display.additive_sprite_supported();
        const float minimum = static_cast<float>(std::min(width_, height_));
        const float atlas_scale = std::clamp(minimum / static_cast<float>(kBaseBufferSize), 1.0F, kMaxAtlasScale);
        for (unsigned s = 0; s < kSizeCount; ++s)
            diameters_[s] = std::min(kMaxDiameter, static_cast<unsigned>(Round(kBaseDiameters[s] * atlas_scale)));
        world_.extent = {kBoxHalfSize * static_cast<float>(width_) / minimum,
                         kBoxHalfSize * static_cast<float>(height_) / minimum,
                         kDepthOptions[kDefaultDepthOption] * 0.5F};
        world_.SetCount(kBallOptions[kDefaultBallOption]);
        focal_ = minimum * 0.5F * kNear / kBoxHalfSize;
        BuildAtlas();
        if (!resources_.UploadTexture(kTextureSlot, kAtlasWidth, kAtlasHeight, mp::RasterLayout::kColumnMajor, atlas_))
            return 1;
        BuildPalette();
        if (!resources_.UploadLitPalette(kPaletteSlot, kPaletteRows, palette_)) return 1;
        BuildWireframe();
        LayoutUi();
        OpenSensors();
        world_.Reset();
        LogSetup(display);

        last_frame_us_ = app_.clock().Now().microseconds();
        simulated_us_ = last_frame_us_;
        window_start_us_ = last_frame_us_;
        for (;;) {
            if (!DrainEvents()) break;
            uint32_t index = 0;
            const uint64_t wait_started_us = app_.clock().Now().microseconds();
            while (!surface_.AcquireFree(index)) {
                if (!HandleEvent(app_.WaitEvent())) return 0;
            }
            const uint64_t frame_started_us = app_.clock().Now().microseconds();
            wait_us_ += frame_started_us - wait_started_us;

            Sample(frame_started_us);
            Advance(frame_started_us);
            const uint64_t physics_done_us = app_.clock().Now().microseconds();
            BuildFrame();
            const uint64_t geometry_done_us = app_.clock().Now().microseconds();
            bool drawn = false;
            auto result = surface_.Update(index, [&](mp::RasterDrawList& list) { drawn = Draw(list, index); });
            const uint64_t update_done_us = app_.clock().Now().microseconds();
            if (!result || !drawn || !surface_.Present(index)) {
                app_.log().Error("gravity-balls: frame rejected by the Host");
                return 2;
            }
            const uint64_t presented_us = app_.clock().Now().microseconds();
            physics_us_ += physics_done_us - frame_started_us;
            geometry_us_ += geometry_done_us - physics_done_us;
            update_us_ += update_done_us - geometry_done_us;
            present_us_ += presented_us - update_done_us;
            if (presented_us - last_frame_us_ > 50000) ++slow_frames_;
            last_frame_us_ = presented_us;
            ++frames_;
            if (frames_ == kStatsWindow) LogWindow(presented_us);
        }
        surface_.Reset();
        return 0;
    }

   private:
    struct DirtyList final {
        mp::Rect rects[kMaxDirty]{};
        unsigned count{};
    };
    struct SettingRow final {
        mp::Point label{}, value{};
        mp::Rect minus{}, plus{};
    };

    // ---- events -----------------------------------------------------------
    bool DrainEvents() {
        mp::Event event;
        while (app_.PollEvent(event)) {
            if (!HandleEvent(event)) return false;
        }
        return true;
    }
    bool HandleEvent(const mp::Event& event) {
        if (event.type() == mp::EventType::kStop) return false;
        if (event.type() == mp::EventType::kResume) {
            // Buffers may hold anything after a pause; time did not pass for the balls.
            for (auto& ready : buffer_ready_) ready = false;
            accumulator_ = 0;
            simulated_us_ = app_.clock().Now().microseconds();
            forces_ = {};
            angular_prev_ = {};
        }
        if (auto* touch = event.touch(); touch && touch->phase() == mp::TouchPhase::kDown) {
            // Touch arrives in logical display pixels; the UI lives in buffer pixels.
            OnTap(Round(static_cast<float>(touch->x()) * touch_scale_x_),
                  Round(static_cast<float>(touch->y()) * touch_scale_y_));
        }
        return true;
    }
    void OnTap(int x, int y) {
        if (menu_button_.contains(x, y)) {
            menu_open_ = !menu_open_;
            return;
        }
        if (menu_open_) {
            if (!panel_rect_.contains(x, y)) {
                menu_open_ = false;
                return;
            }
            for (unsigned row = 0; row < kSettingRows; ++row) {
                if (rows_[row].minus.contains(x, y)) Adjust(row, -1);
                if (rows_[row].plus.contains(x, y)) Adjust(row, +1);
            }
            return;
        }
        if (y < height_ / 8)
            world_.Reset();
        else
            world_.Kick();
    }
    // Moves one setting by `delta` options and applies it to the world.
    void Adjust(unsigned row, int delta) {
        const auto step = [delta](unsigned& option, unsigned count) {
            const int next = static_cast<int>(option) + delta;
            if (next < 0 || next >= static_cast<int>(count)) return false;
            option = static_cast<unsigned>(next);
            return true;
        };
        switch (row) {
            case 0:
                if (step(ball_option_, sizeof(kBallOptions) / sizeof(kBallOptions[0])))
                    world_.SetCount(kBallOptions[ball_option_]);
                break;
            case 1:
                if (step(depth_option_, sizeof(kDepthOptions) / sizeof(kDepthOptions[0]))) {
                    world_.extent.z = kDepthOptions[depth_option_] * 0.5F;
                    BuildWireframe();
                }
                break;
            default:
                step(gravity_option_, sizeof(kGravityOptions) / sizeof(kGravityOptions[0]));
                break;
        }
    }
    [[nodiscard]] float GravityScale() const { return World::kMetersToUnits * kGravityOptions[gravity_option_]; }

    // ---- sensors ----------------------------------------------------------
    void OpenSensors() {
        auto devices = app_.devices().List(mp::DeviceKind::kSensor);
        if (!devices) return;
        for (auto id : *devices) {
            auto info = app_.sensors().GetInfo(id);
            if (!info) continue;
            auto interval = mp::Duration::Milliseconds(5);
            if (interval < info->minimum_interval) interval = info->minimum_interval;
            if (interval > info->maximum_interval) interval = info->maximum_interval;
            if (info->kind == mp::SensorKind::kAcceleration && !accelerometer_.valid()) {
                auto sensor = app_.sensors().Open<mp::Acceleration>(id);
                if (sensor && sensor->SetSampleInterval(interval)) accelerometer_ = std::move(*sensor);
            } else if (info->kind == mp::SensorKind::kAngularVelocity && !gyroscope_.valid()) {
                auto sensor = app_.sensors().Open<mp::AngularVelocity>(id);
                if (sensor && sensor->SetSampleInterval(interval)) gyroscope_ = std::move(*sensor);
            }
        }
    }
    // Sensor axes follow the Tilt convention: screen right is -X, screen down
    // is +Y. World y points up, so gravity in world space is (-ax, -ay, +az)
    // up to kDepthAxisSign; the gyroscope shares the sensor frame.
    void Sample(uint64_t now_us) {
        if (accelerometer_.valid()) {
            auto sample = accelerometer_.Read();
            if (sample && sample->timestamp.microseconds() != last_accel_us_) {
                const auto a = sample->value.meters_per_second_squared;
                if (std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)) {
                    last_accel_us_ = sample->timestamp.microseconds();
                    last_accel_received_us_ = now_us;
                    Vector g{-a.x, -a.y, kDepthAxisSign * a.z};
                    const float magnitude2 = g.Dot(g);
                    if (magnitude2 > kMaxGravity * kMaxGravity) g = g * (kMaxGravity / std::sqrt(magnitude2));
                    g = g * GravityScale();
                    // Light smoothing: one sample of lag, keeps shakes sharp.
                    forces_.gravity = forces_.gravity + (g - forces_.gravity) * 0.6F;
                    ++imu_samples_;
                    imu_live_ = true;
                }
            }
        }
        if (imu_live_ && now_us - last_accel_received_us_ > kSensorTimeoutUs) {
            imu_live_ = false;
        }
        if (!imu_live_) {
            const Vector rest{0, 0, 9.8F * GravityScale()};
            forces_.gravity = forces_.gravity + (rest - forces_.gravity) * 0.1F;
        }
        if (gyroscope_.valid()) {
            auto sample = gyroscope_.Read();
            if (sample && sample->timestamp.microseconds() != last_gyro_us_) {
                const auto w = sample->value.radians_per_second;
                if (std::isfinite(w.x) && std::isfinite(w.y) && std::isfinite(w.z)) {
                    const Vector omega{w.x, w.y, -w.z};
                    const float dt = last_gyro_us_ == 0
                                         ? 0.0F
                                         : static_cast<float>(sample->timestamp.microseconds() - last_gyro_us_) * 1e-6F;
                    if (dt > 1e-4F && dt < 0.2F) {
                        const Vector alpha = (omega - angular_prev_) * (1.0F / dt);
                        forces_.angular_acceleration =
                            forces_.angular_acceleration + (alpha - forces_.angular_acceleration) * 0.5F;
                    }
                    angular_prev_ = omega;
                    forces_.angular_velocity = omega;
                    last_gyro_us_ = sample->timestamp.microseconds();
                    last_gyro_received_us_ = now_us;
                    ++gyro_samples_;
                }
            }
            if (now_us - last_gyro_received_us_ > kSensorTimeoutUs) {
                forces_.angular_velocity = {};
                forces_.angular_acceleration = {};
            }
        }
    }
    void Advance(uint64_t now_us) {
        const float dt = std::min(static_cast<float>(now_us - simulated_us_) * 1e-6F, 0.1F);
        simulated_us_ = now_us;
        accumulator_ += dt;
        unsigned steps = 0;
        while (accumulator_ >= World::kStep && steps < kMaxStepsPerFrame) {
            world_.Step(forces_);
            accumulator_ -= World::kStep;
            ++steps;
        }
        if (accumulator_ >= World::kStep) {
            // A slow frame drops simulation time rather than compounding work.
            dropped_steps_ += static_cast<unsigned>(accumulator_ / World::kStep);
            accumulator_ = 0;
        }
        steps_ += steps;
    }

    // ---- resources --------------------------------------------------------
    // Ball shading is baked per texel as an intensity 1..252 (0 = outside).
    // The rim's coverage multiplies the intensity, which on a black
    // background is exactly alpha blending, so the edge is anti-aliased for
    // free. Light comes from the upper left, slightly in front.
    void BuildAtlas() {
        constexpr float kLight[3] = {-0.45F, -0.60F, 0.66F};          // texture y is down; +z faces the viewer
        constexpr float kHalfVector[3] = {-0.248F, -0.331F, 0.911F};  // normalize(light + (0, 0, 1))
        unsigned offset = 0;
        for (unsigned s = 0; s < kSizeCount; ++s) {
            const unsigned d = diameters_[s];
            size_offset_[s] = offset;
            const float radius = static_cast<float>(d) * 0.5F - 0.5F;
            const float centre = static_cast<float>(d) * 0.5F;
            for (unsigned u = 0; u < d; ++u)
                for (unsigned v = 0; v < d; ++v) {
                    const float dx = static_cast<float>(u) + 0.5F - centre;
                    const float dy = static_cast<float>(v) + 0.5F - centre;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    const float coverage = std::clamp(radius + 0.5F - distance, 0.0F, 1.0F);
                    uint8_t texel = 0;
                    if (coverage > 0) {
                        const float nx = std::clamp(dx / radius, -1.0F, 1.0F);
                        const float ny = std::clamp(dy / radius, -1.0F, 1.0F);
                        const float nz = std::sqrt(std::max(0.0F, 1.0F - nx * nx - ny * ny));
                        const float diffuse = std::max(0.0F, nx * kLight[0] + ny * kLight[1] + nz * kLight[2]);
                        float specular =
                            std::max(0.0F, nx * kHalfVector[0] + ny * kHalfVector[1] + nz * kHalfVector[2]);
                        for (unsigned power = 0; power < 5; ++power) specular *= specular;  // ^32
                        const float intensity = std::min(1.0F, 0.14F + 0.70F * diffuse + 0.32F * specular);
                        texel = static_cast<uint8_t>(1 + Round(coverage * intensity * (kMaxShade - 1)));
                    }
                    atlas_[(offset + u) * kAtlasHeight + v] = texel;
                }
            offset += d;
        }
        // Soft radial blob for glows and light pools.
        const float blob_radius = static_cast<float>(kBlobSize) * 0.5F;
        for (unsigned u = 0; u < kBlobSize; ++u)
            for (unsigned v = 0; v < kBlobSize; ++v) {
                const float dx = (static_cast<float>(u) + 0.5F - blob_radius) / blob_radius;
                const float dy = (static_cast<float>(v) + 0.5F - blob_radius) / blob_radius;
                const float r = std::sqrt(dx * dx + dy * dy);
                uint8_t texel = 0;
                if (r < 1.0F) {
                    const float falloff = (1.0F - r) * (1.0F - r);
                    const int value = Round(falloff * (kMaxShade - 1));
                    texel = value > 0 ? static_cast<uint8_t>(1 + value) : 0;
                }
                atlas_[(offset + u) * kAtlasHeight + v] = texel;
            }
        blob_offset_ = offset;
    }
    // Rows 0..7: ball colours (intensity ramps to the colour, the top fifth
    // blends toward white for the highlight); entries 253/254 of those rows
    // hold the wireframe shades, darkening with the row for depth fog.
    // Rows 8..15: glow colours, rows 16..23: light-pool colours, both dim
    // ramps meant for additive drawing.
    void BuildPalette() {
        for (unsigned c = 0; c < kColors; ++c) {
            const float r = kPalette[c][0], g = kPalette[c][1], b = kPalette[c][2];
            for (unsigned i = 1; i <= kMaxShade; ++i) {
                const float t = static_cast<float>(i) / kMaxShade;
                float sr = r, sg = g, sb = b;
                if (t <= 0.78F) {
                    const float s = t / 0.78F;
                    sr *= s;
                    sg *= s;
                    sb *= s;
                } else {
                    // Highlight: blend only part of the way to white so a
                    // cluster of balls does not turn into a white patch.
                    const float s = (t - 0.78F) / 0.22F * 0.55F;
                    sr += (255 - r) * s;
                    sg += (255 - g) * s;
                    sb += (255 - b) * s;
                }
                palette_[c * 256 + i] = Rgb565(sr, sg, sb);
                palette_[(kGlowRow + c) * 256 + i] = Rgb565(r * 0.3F * t, g * 0.3F * t, b * 0.3F * t);
                palette_[(kPoolRow + c) * 256 + i] = Rgb565(r * 0.28F * t, g * 0.28F * t, b * 0.28F * t);
            }
            const float depth = 0.3F + 0.7F * static_cast<float>(c) / (kColors - 1);
            palette_[c * 256 + kGridIndex] = Rgb565(26 * depth, 44 * depth, 68 * depth);
            palette_[c * 256 + kWireIndex] = Rgb565(70 * depth, 125 * depth, 200 * depth);
        }
    }
    // Box edges in world space (relative to the box centre): the far
    // rectangle, the four depth edges, two depth rulers and a far-wall grid.
    void BuildWireframe() {
        const Vector e = world_.extent;
        unsigned n = 0;
        const auto add = [&](Vector a, Vector b, uint8_t index) {
            if (n < kWireLines) wires_[n++] = {a, b, index};
        };
        const auto rectangle = [&](float z, uint8_t index) {
            add({-e.x, -e.y, z}, {e.x, -e.y, z}, index);
            add({e.x, -e.y, z}, {e.x, e.y, z}, index);
            add({e.x, e.y, z}, {-e.x, e.y, z}, index);
            add({-e.x, e.y, z}, {-e.x, -e.y, z}, index);
        };
        rectangle(e.z, kWireIndex);
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) add({sx * e.x, sy * e.y, -e.z}, {sx * e.x, sy * e.y, e.z}, kWireIndex);
        rectangle(-e.z + 2 * e.z / 3, kGridIndex);
        rectangle(-e.z + 4 * e.z / 3, kGridIndex);
        for (int k = -1; k <= 1; ++k) {
            add({k * e.x * 0.5F, -e.y, e.z}, {k * e.x * 0.5F, e.y, e.z}, kGridIndex);
            add({-e.x, k * e.y * 0.5F, e.z}, {e.x, k * e.y * 0.5F, e.z}, kGridIndex);
        }
        wire_count_ = n;
    }
    // Fixed rectangles for the HUD, the menu button and the settings panel.
    void LayoutUi() {
        auto title = app_.renderer().MeasureText("GRAVITY BALLS", mp::SystemFont::kMedium);
        auto status = app_.renderer().MeasureText("tap outside to close", mp::SystemFont::kSmall);
        auto menu = app_.renderer().MeasureText("MENU", mp::SystemFont::kSmall);
        auto label = app_.renderer().MeasureText("GRAVITY", mp::SystemFont::kMedium);
        auto value = app_.renderer().MeasureText("x1.5", mp::SystemFont::kMedium);
        auto sign = app_.renderer().MeasureText("+", mp::SystemFont::kMedium);
        const int title_h = title ? static_cast<int>(title->height) : 24;
        const int small_h = status ? static_cast<int>(status->height) : 18;
        const int medium_h = label ? static_cast<int>(label->height) : 24;
        menu_text_h_ = small_h;
        const int menu_w = menu ? static_cast<int>(menu->width) : 40;
        const int label_w = label ? static_cast<int>(label->width) : 90;
        const int value_w = value ? static_cast<int>(value->width) : 48;
        sign_w_ = sign ? static_cast<int>(sign->width) : 12;
        sign_h_ = sign ? static_cast<int>(sign->height) : medium_h;
        // The title sits inside the safe area (round panels report corner
        // insets); the safe area arrives in logical display pixels and is
        // converted to buffer pixels here. The menu button hugs the buffer's
        // top-right corner instead, where the tab reads naturally on the
        // round screen.
        const mp::Rect logical_safe = app_.renderer().info().safe_area();
        const mp::Rect safe{Round(static_cast<float>(logical_safe.x) * touch_scale_x_),
                            Round(static_cast<float>(logical_safe.y) * touch_scale_y_),
                            Round(static_cast<float>(logical_safe.width) * touch_scale_x_),
                            Round(static_cast<float>(logical_safe.height) * touch_scale_y_)};
        const int button_h = std::max(small_h + 24, 44);
        title_rect_ = {safe.x + 8, safe.y + 6, std::min(safe.width - 16, 260), title_h + 2};
        menu_button_ = {width_ - menu_w - 44, 4, menu_w + 40, button_h};

        // Panel: content-sized and centred, leaving at least ~10% of the
        // buffer free on each side so a tap outside can close it. Buttons and
        // padding scale with the buffer so they stay finger-sized on the panel
        // whether the buffer is upscaled or native.
        const float minimum = static_cast<float>(std::min(width_, height_));
        const float ui = std::clamp(minimum / static_cast<float>(kBaseBufferSize), 0.75F, 1.5F);
        const int pad = std::max(8, Round(16.0F * ui));
        const int button = std::max(medium_h + 20, Round(56.0F * ui));
        const int row_h = button + pad;
        const int needed = 5 * pad + label_w + value_w + 2 * button;
        const int panel_w = std::min(needed, width_ - 2 * Round(48.0F * ui));
        const int title_h_row = medium_h + pad;
        const int hint_h_row = small_h + pad;
        const int panel_h = 2 * pad + title_h_row + row_h * static_cast<int>(kSettingRows) + hint_h_row;
        panel_rect_ = {(width_ - panel_w) / 2, (height_ - panel_h) / 2, panel_w, panel_h};
        const int right = panel_rect_.x + panel_rect_.width - pad;
        panel_title_ = {panel_rect_.x + pad, panel_rect_.y + pad + (title_h_row - medium_h) / 2};
        for (unsigned row = 0; row < kSettingRows; ++row) {
            SettingRow& r = rows_[row];
            const int top = panel_rect_.y + pad + title_h_row + row_h * static_cast<int>(row);
            r.label = {panel_rect_.x + pad, top + (row_h - medium_h) / 2};
            r.plus = {right - button, top + (row_h - button) / 2, button, button};
            r.value = {r.plus.x - pad - value_w, r.label.y};
            r.minus = {r.value.x - pad - button, r.plus.y, button, button};
        }
        const int hint_top = panel_rect_.y + pad + title_h_row + row_h * static_cast<int>(kSettingRows);
        hint_ = {panel_rect_.x + pad, hint_top + (hint_h_row - small_h) / 2};
    }
    static void FormatValue(mp::FixedString<16>& text, unsigned row, unsigned option) {
        text.Clear();
        if (row == 0) {
            text.AppendUint(kBallOptions[option]);
        } else if (row == 1) {
            text.AppendUint(static_cast<uint64_t>(Round(kDepthOptions[option])));
        } else {
            text.Append("x");
            const int tenths = Round(kGravityOptions[option] * 10.0F);
            text.AppendUint(static_cast<uint64_t>(tenths / 10));
            text.Append(".");
            text.AppendUint(static_cast<uint64_t>(tenths % 10));
        }
    }

    // ---- geometry ---------------------------------------------------------
    [[nodiscard]] float ViewDepth(float z) const { return z + world_.extent.z + kNear; }
    void Project(Vector p, float& x, float& y, float& depth) const {
        depth = ViewDepth(p.z);
        const float scale = focal_ / depth;
        x = static_cast<float>(width_) * 0.5F + p.x * scale;
        y = static_cast<float>(height_) * 0.5F - p.y * scale;
    }
    static mp::Rect Square(float cx, float cy, float radius) {
        const int size = std::max(2, Round(radius * 2));
        return {Round(cx - static_cast<float>(size) * 0.5F), Round(cy - static_cast<float>(size) * 0.5F), size, size};
    }
    static bool Intersects(mp::Rect a, mp::Rect b) {
        return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
    }
    static mp::Rect Union(mp::Rect a, mp::Rect b) {
        const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
        const int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
        return {x0, y0, x1 - x0, y1 - y0};
    }
    void BuildFrame() {
        const float far_depth = ViewDepth(world_.extent.z);
        for (unsigned i = 0; i < world_.count; ++i) {
            const auto& ball = world_.balls[i];
            BallDraw& d = draws_[i];
            float x{}, y{};
            Project(ball.position, x, y, d.depth);
            const float radius_px = ball.radius * focal_ / d.depth;
            d.ball = Square(x, y, radius_px);
            d.glow = Square(x, y, radius_px * 1.6F);
            unsigned best = 0;
            for (unsigned s = 1; s < kSizeCount; ++s)
                if (Distance(static_cast<int>(diameters_[s]), d.ball.width) <
                    Distance(static_cast<int>(diameters_[best]), d.ball.width))
                    best = s;
            d.size = static_cast<uint8_t>(best);
            d.color = static_cast<uint8_t>(i % kColors);
            // Light pool: the ball's footprint on the far wall, wider and
            // fainter the higher the ball floats above it.
            const float height = (world_.extent.z - ball.position.z) / (2 * world_.extent.z);
            const float pool_scale = focal_ / far_depth;
            const float px = static_cast<float>(width_) * 0.5F + ball.position.x * pool_scale;
            const float py = static_cast<float>(height_) * 0.5F - ball.position.y * pool_scale;
            d.pool = Square(px, py, ball.radius * pool_scale * (0.9F + 1.5F * height));
            // Resting on the far wall the pool sits entirely under its ball.
            d.pool_visible = height > 0.04F;
            order_[i] = static_cast<uint8_t>(i);
        }
        // Far to near, so nearer balls overwrite farther ones.
        for (unsigned i = 1; i < world_.count; ++i) {
            const uint8_t key = order_[i];
            unsigned j = i;
            while (j > 0 && draws_[order_[j - 1]].depth < draws_[key].depth) {
                order_[j] = order_[j - 1];
                --j;
            }
            order_[j] = key;
        }
    }

    // ---- drawing ----------------------------------------------------------
    bool WireQuad(mp::RasterDrawList& list, const WireLine& line) {
        float x0{}, y0{}, z0{}, x1{}, y1{}, z1{};
        Project(line.a, x0, y0, z0);
        Project(line.b, x1, y1, z1);
        float dx = x1 - x0, dy = y1 - y0;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.5F) return true;
        dx /= length;
        dy /= length;
        // Two pixels wide: half a pixel each side would miss pixel centres.
        const float nx = -dy, ny = dx;
        const auto light = [&](float depth) {
            const float t = (depth - kNear) / (2 * world_.extent.z);
            return static_cast<uint8_t>(std::clamp(Round(7.0F - 7.0F * t), 0, 7));
        };
        const mp::RasterVertex corners[4] = {mp::RasterVertex::At(x0 + nx, y0 + ny, 0, 0, light(z0)),
                                             mp::RasterVertex::At(x1 + nx, y1 + ny, 0, 0, light(z1)),
                                             mp::RasterVertex::At(x1 - nx, y1 - ny, 0, 0, light(z1)),
                                             mp::RasterVertex::At(x0 - nx, y0 - ny, 0, 0, light(z0))};
        ++records_;
        return list.FlatQuad(corners, line.index);
    }
    bool Draw(mp::RasterDrawList& list, uint32_t index) {
        list.SetPalette(kPaletteSlot);
        const mp::Color black = mp::Color::Rgb(0, 0, 0);
        DirtyList& dirty = dirty_[index];
        if (!buffer_ready_[index]) {
            if (!list.FillRect({0, 0, width_, height_}, black)) return false;
            clear_px_ += static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_);
            ++records_;
            buffer_ready_[index] = true;
        } else {
            // Many overlapping rectangles cost more than one full clear.
            uint64_t area = 0;
            for (unsigned i = 0; i < dirty.count; ++i)
                area += static_cast<uint64_t>(dirty.rects[i].width) * static_cast<uint64_t>(dirty.rects[i].height);
            const uint64_t full = static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_);
            if (area * 10 > full * 6) {
                if (!list.FillRect({0, 0, width_, height_}, black)) return false;
                clear_px_ += full;
                ++records_;
            } else {
                for (unsigned i = 0; i < dirty.count; ++i) {
                    const mp::Rect r = dirty.rects[i];
                    if (r.empty()) continue;
                    if (!list.FillRect(r, black)) return false;
                    clear_px_ += static_cast<uint64_t>(r.width) * static_cast<uint64_t>(r.height);
                    ++records_;
                }
            }
        }
        dirty.count = 0;
        if (polygons_) {
            for (unsigned i = 0; i < wire_count_; ++i)
                if (!WireQuad(list, wires_[i])) return false;
        }
        const uint16_t blob = static_cast<uint16_t>(blob_offset_);
        if (additive_) {
            for (unsigned i = 0; i < world_.count; ++i) {
                const BallDraw& d = draws_[i];
                if (!d.pool_visible) continue;
                if (!list.AdditiveSprite(d.pool, kTextureSlot, static_cast<uint8_t>(kPoolRow + d.color), blob, 0,
                                         kBlobSize, kBlobSize))
                    return false;
                sprite_px_ += static_cast<uint64_t>(d.pool.width) * static_cast<uint64_t>(d.pool.height);
                ++records_;
                // A pool under its own ball shares one dirty rectangle with it.
                if (!Intersects(d.pool, Union(d.ball, d.glow))) dirty.rects[dirty.count++] = d.pool;
            }
        }
        // All glows go down before any ball so they only light the background;
        // an opaque ball drawn later hides its neighbours' glow instead of
        // having it added on top of its highlight.
        if (additive_) {
            for (unsigned i = 0; i < world_.count; ++i) {
                const BallDraw& d = draws_[i];
                if (!list.AdditiveSprite(d.glow, kTextureSlot, static_cast<uint8_t>(kGlowRow + d.color), blob, 0,
                                         kBlobSize, kBlobSize))
                    return false;
                sprite_px_ += static_cast<uint64_t>(d.glow.width) * static_cast<uint64_t>(d.glow.height);
                ++records_;
            }
        }
        for (unsigned k = 0; k < world_.count; ++k) {
            const BallDraw& d = draws_[order_[k]];
            mp::Rect touched = d.ball;
            if (additive_) touched = Union(touched, d.glow);
            if (additive_ && d.pool_visible && Intersects(d.pool, touched)) touched = Union(touched, d.pool);
            const auto diameter = static_cast<uint16_t>(diameters_[d.size]);
            if (!list.Sprite(d.ball, kTextureSlot, d.color, static_cast<uint16_t>(size_offset_[d.size]), 0, diameter,
                             diameter))
                return false;
            sprite_px_ += static_cast<uint64_t>(d.ball.width) * static_cast<uint64_t>(d.ball.height);
            ++records_;
            dirty.rects[dirty.count++] = touched;
        }
        const auto white = mp::Color::Rgb(220, 234, 250);
        if (!list.Text({title_rect_.x, title_rect_.y}, "GRAVITY BALLS", white, mp::SystemFont::kMedium)) return false;
        ++records_;
        dirty.rects[dirty.count++] = title_rect_;
        if (!DrawMenu(list, dirty)) return false;
        return true;
    }
    bool DrawMenu(mp::RasterDrawList& list, DirtyList& dirty) {
        const auto frame = mp::Color::Rgb(70, 125, 200);
        const auto text = mp::Color::Rgb(220, 234, 250);
        const auto dim = mp::Color::Rgb(120, 140, 170);
        const auto button_fill = mp::Color::Rgb(34, 48, 80);
        // Always-visible button, brighter while the panel is open.
        if (!list.FillRect(menu_button_, menu_open_ ? button_fill : mp::Color::Rgb(16, 22, 38), 255) ||
            !list.Text({menu_button_.x + 20, menu_button_.y + (menu_button_.height - menu_text_h_) / 2}, "MENU",
                       menu_open_ ? text : dim, mp::SystemFont::kSmall))
            return false;
        records_ += 2;
        dirty.rects[dirty.count++] = menu_button_;
        if (!menu_open_) return true;

        const mp::Rect p = panel_rect_;
        if (!list.FillRect(p, mp::Color::Rgb(8, 12, 24), 220) || !list.FillRect({p.x, p.y, p.width, 2}, frame) ||
            !list.FillRect({p.x, p.y + p.height - 2, p.width, 2}, frame) ||
            !list.FillRect({p.x, p.y, 2, p.height}, frame) ||
            !list.FillRect({p.x + p.width - 2, p.y, 2, p.height}, frame))
            return false;
        if (!list.Text(panel_title_, "SETTINGS", text, mp::SystemFont::kMedium)) return false;
        records_ += 6;
        static constexpr const char* kLabels[kSettingRows] = {"BALLS", "DEPTH", "GRAVITY"};
        const unsigned options[kSettingRows] = {ball_option_, depth_option_, gravity_option_};
        for (unsigned row = 0; row < kSettingRows; ++row) {
            const SettingRow& r = rows_[row];
            mp::FixedString<16> value;
            FormatValue(value, row, options[row]);
            const auto glyph = [&](mp::Rect box, const char* sign) {
                return list.FillRect(box, button_fill) &&
                       list.Text({box.x + (box.width - sign_w_) / 2, box.y + (box.height - sign_h_) / 2}, sign, text,
                                 mp::SystemFont::kMedium);
            };
            if (!list.Text(r.label, kLabels[row], dim, mp::SystemFont::kMedium) || !glyph(r.minus, "-") ||
                !list.Text(r.value, value.c_str(), text, mp::SystemFont::kMedium) || !glyph(r.plus, "+"))
                return false;
            records_ += 6;
        }
        if (!list.Text(hint_, "tap outside to close", dim, mp::SystemFont::kSmall)) return false;
        ++records_;
        dirty.rects[dirty.count++] = p;
        return true;
    }

    // ---- telemetry --------------------------------------------------------
    void LogSetup(const mp::RendererInfo& display) {
        Line msg;
        msg.Append("gravity-balls: buffer ");
        msg.AppendUint(static_cast<uint32_t>(width_));
        msg.Append("x");
        msg.AppendUint(static_cast<uint32_t>(height_));
        msg.Append(" x");
        msg.AppendUint(display.physical_width() / surface_.buffer_width());
        msg.Append(surface_.direct_scanout() ? " direct scanout" : " composited");
        msg.Append(", logical ");
        msg.AppendUint(display.width());
        msg.Append("x");
        msg.AppendUint(display.height());
        msg.Append(", panel max ");
        msg.AppendUint(surface_.max_full_frame_fps());
        msg.Append(" fps, polygons=");
        msg.AppendUint(polygons_ ? 1U : 0U);
        msg.Append(" additive=");
        msg.AppendUint(additive_ ? 1U : 0U);
        msg.Append(" accelerometer=");
        msg.AppendUint(accelerometer_.valid() ? 1U : 0U);
        msg.Append(" gyroscope=");
        msg.AppendUint(gyroscope_.valid() ? 1U : 0U);
        msg.Append(" wires=");
        msg.AppendUint(wire_count_);
        app_.log().Info(msg.c_str());
    }
    void LogWindow(uint64_t now_us) {
        const uint64_t elapsed_us = now_us - window_start_us_;
        Line msg;
        msg.Append("gravity-balls: frames=");
        msg.AppendUint(frames_);
        msg.Append(" fps_x100=");
        msg.AppendUint(elapsed_us == 0 ? 0 : frames_ * 100000000ULL / elapsed_us);
        msg.Append(" wait_us=");
        msg.AppendUint(wait_us_ / frames_);
        msg.Append(" physics_us=");
        msg.AppendUint(physics_us_ / frames_);
        msg.Append(" geometry_us=");
        msg.AppendUint(geometry_us_ / frames_);
        msg.Append(" update_us=");
        msg.AppendUint(update_us_ / frames_);
        msg.Append(" present_us=");
        msg.AppendUint(present_us_ / frames_);
        msg.Append(" records=");
        msg.AppendUint(records_ / frames_);
        msg.Append(" clear_px=");
        msg.AppendUint(clear_px_ / frames_);
        msg.Append(" sprite_px=");
        msg.AppendUint(sprite_px_ / frames_);
        app_.log().Info(msg.c_str());
        // Second line: simulation state, kept separate so neither line is
        // truncated by the Host log limit.
        msg.Clear();
        msg.Append("gravity-balls: sim balls=");
        msg.AppendUint(world_.count);
        msg.Append(" depth_x10=");
        msg.AppendUint(static_cast<uint64_t>(Round(world_.extent.z * 20.0F)));
        msg.Append(" gravity_x10=");
        msg.AppendUint(static_cast<uint64_t>(Round(kGravityOptions[gravity_option_] * 10.0F)));
        msg.Append(" steps_x100=");
        msg.AppendUint(steps_ * 100ULL / frames_);
        msg.Append(" dropped=");
        msg.AppendUint(dropped_steps_);
        msg.Append(" slow=");
        msg.AppendUint(slow_frames_);
        float z_sum = 0, speed_sum = 0;
        for (unsigned i = 0; i < world_.count; ++i) {
            z_sum += world_.balls[i].position.z;
            speed_sum += std::sqrt(world_.balls[i].velocity.Dot(world_.balls[i].velocity));
        }
        const float count = static_cast<float>(std::max(1U, world_.count));
        msg.Append(" z_avg_x100=");
        msg.AppendInt(Round(z_sum / count * 100));
        msg.Append(" speed_avg_x100=");
        msg.AppendUint(static_cast<uint64_t>(Round(speed_sum / count * 100)));
        msg.Append(" imu_hz_x10=");
        msg.AppendUint(elapsed_us == 0 ? 0 : (imu_samples_ - window_imu_samples_) * 10000000ULL / elapsed_us);
        msg.Append(" gyro_hz_x10=");
        msg.AppendUint(elapsed_us == 0 ? 0 : (gyro_samples_ - window_gyro_samples_) * 10000000ULL / elapsed_us);
        app_.log().Info(msg.c_str());
        window_start_us_ = now_us;
        window_imu_samples_ = imu_samples_;
        window_gyro_samples_ = gyro_samples_;
        frames_ = 0;
        wait_us_ = physics_us_ = geometry_us_ = update_us_ = present_us_ = 0;
        records_ = clear_px_ = sprite_px_ = steps_ = 0;
        dropped_steps_ = slow_frames_ = 0;
    }

    mp::Application app_{};
    mp::HostSurface surface_{};
    mp::RasterResources resources_{};
    mp::Accelerometer accelerometer_{};
    mp::Gyroscope gyroscope_{};
    World world_{};
    Forces forces_{};
    Vector angular_prev_{};
    uint8_t atlas_[kAtlasWidth * kAtlasHeight]{};
    uint16_t palette_[kPaletteRows * 256]{};
    WireLine wires_[kWireLines]{};
    BallDraw draws_[World::kMaxCount]{};
    uint8_t order_[World::kMaxCount]{};
    DirtyList dirty_[kBufferCount]{};
    bool buffer_ready_[kBufferCount]{};
    mp::Rect title_rect_{}, menu_button_{}, panel_rect_{};
    SettingRow rows_[kSettingRows]{};
    mp::Point hint_{}, panel_title_{};
    int menu_text_h_{}, sign_w_{}, sign_h_{};
    unsigned ball_option_ = kDefaultBallOption, depth_option_ = kDefaultDepthOption,
             gravity_option_ = kDefaultGravityOption;
    bool menu_open_{};
    unsigned size_offset_[kSizeCount]{};
    unsigned diameters_[kSizeCount]{};
    unsigned wire_count_{}, blob_offset_{};
    int width_{}, height_{};
    float focal_{}, touch_scale_x_{1.0F}, touch_scale_y_{1.0F};
    bool polygons_{}, additive_{}, imu_live_{};
    float accumulator_{};
    uint64_t last_frame_us_{}, simulated_us_{}, last_accel_us_{}, last_accel_received_us_{}, last_gyro_us_{},
        last_gyro_received_us_{};
    // Telemetry accumulators for the current window.
    uint64_t window_start_us_{};
    uint64_t wait_us_{}, physics_us_{}, geometry_us_{}, update_us_{}, present_us_{};
    uint64_t records_{}, clear_px_{}, sprite_px_{}, steps_{};
    uint64_t imu_samples_{}, gyro_samples_{}, window_imu_samples_{}, window_gyro_samples_{};
    unsigned frames_{}, dropped_steps_{}, slow_frames_{};
};
}  // namespace gravity_balls

int main() { return std::make_unique<gravity_balls::Demo>()->Run(); }
