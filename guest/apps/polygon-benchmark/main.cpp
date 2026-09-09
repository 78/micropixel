// Polygon fill benchmark for the Host raster TRIANGLE/QUAD kernels.
//
// Cycles through fixed phases and logs, every 120 frames, the frame rate, the
// Guest-side render time (which includes the Host kernels: raster records
// execute on the Guest task inside HostSurface::Update) and the pixels that
// were submitted, so that ns/pixel and the affordable overdraw can be read
// off a serial log on any board:
//
//   fill      one full-buffer quad per frame (pure fill cost)
//   quads-1x  random 120 px textured quads covering ~1.0x the buffer
//   quads-1.5x / quads-2x  the same at 1.5x and 2.0x overdraw
//   small-2x  ~24 px quads at 2.0x overdraw (per-polygon overhead)
//   room      a textured box room through the MeshRenderer (camera spins)
//
// Options: --upscale=N (Host enlarges an N times smaller buffer),
// --phase=NAME (stay in one phase), --frames=N (frames per phase, default 240).

#include <stdint.h>

#include <memory>
#include <span>

#include "sdk/mesh_renderer.hpp"
#include "sdk/micropixel.hpp"

namespace {

constexpr uint32_t kBufferCount = 2U;
constexpr uint32_t kTextureSize = 64U;
constexpr uint32_t kLightLevels = 16U;
constexpr uint32_t kStatsWindow = 120U;
constexpr uint8_t kTextureSlot = 0U;
constexpr uint8_t kPaletteSlot = 0U;
constexpr uint32_t kMaxQuads = 2048U;

using Line = micropixel::FixedString<240U>;

enum class Phase : uint8_t { kFill, kQuads1x, kQuads15x, kQuads2x, kSmall2x, kRoom, kCount };

const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::kFill:
            return "fill";
        case Phase::kQuads1x:
            return "quads-1x";
        case Phase::kQuads15x:
            return "quads-1.5x";
        case Phase::kQuads2x:
            return "quads-2x";
        case Phase::kSmall2x:
            return "small-2x";
        case Phase::kRoom:
            return "room";
        default:
            return "?";
    }
}

bool SameString(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
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

struct Options final {
    uint32_t upscale{1U};
    uint32_t frames_per_phase{240U};
    int fixed_phase{-1};
};

Options ParseOptions(const micropixel::LaunchArguments& args) {
    Options options{};
    options.upscale = ParseUint(args.FindValue("--upscale"), 1U);
    if (options.upscale < 1U || options.upscale > 4U) options.upscale = 1U;
    options.frames_per_phase = ParseUint(args.FindValue("--frames"), 240U);
    if (options.frames_per_phase < kStatsWindow) options.frames_per_phase = kStatsWindow;
    const char* phase = args.FindValue("--phase");
    if (phase != nullptr) {
        for (int index = 0; index < static_cast<int>(Phase::kCount); ++index) {
            if (SameString(phase, PhaseName(static_cast<Phase>(index)))) options.fixed_phase = index;
        }
    }
    return options;
}

class Rng final {
   public:
    explicit Rng(uint32_t seed) : state_(seed == 0U ? 0x9E3779B9U : seed) {}
    uint32_t Next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return state_;
    }
    int Range(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint32_t>(high - low + 1)); }

   private:
    uint32_t state_;
};

uint16_t Rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return static_cast<uint16_t>(((r & 0xF8U) << 8U) | ((g & 0xFCU) << 3U) | (b >> 3U));
}

// Brick texture: index 1..8 mortar shades, 9..255 brick shades.
void BuildTexture(uint8_t* texels) {
    for (uint32_t y = 0U; y < kTextureSize; ++y) {
        for (uint32_t x = 0U; x < kTextureSize; ++x) {
            const uint32_t row = y / 16U;
            const uint32_t shifted = (x + (row & 1U) * 16U) % kTextureSize;
            const bool mortar = (y % 16U) < 2U || (shifted % 32U) < 2U;
            uint8_t index;
            if (mortar) {
                index = static_cast<uint8_t>(1U + ((x * 7U + y * 3U) % 8U));
            } else {
                const uint32_t noise = (x * 31U + y * 17U + (x ^ y) * 5U) % 60U;
                index = static_cast<uint8_t>(9U + ((x / 32U + row) % 3U) * 60U + noise);
            }
            texels[y * kTextureSize + x] = index;
        }
    }
}

void BuildPalette(uint16_t* entries) {
    for (uint32_t level = 0U; level < kLightLevels; ++level) {
        const uint32_t scale = level * 255U / (kLightLevels - 1U);
        uint16_t* row = entries + level * micropixel::RasterResources::kPaletteEntries;
        for (uint32_t index = 0U; index < micropixel::RasterResources::kPaletteEntries; ++index) {
            uint32_t r, g, b;
            if (index == 0U) {
                r = g = b = 0U;
            } else if (index < 9U) {
                r = g = b = 90U + index * 8U;
            } else {
                const uint32_t family = (index - 9U) / 60U;
                const uint32_t shade = (index - 9U) % 60U;
                r = (family == 0U ? 150U : (family == 1U ? 170U : 130U)) + shade;
                g = (family == 0U ? 70U : (family == 1U ? 90U : 60U)) + shade / 2U;
                b = (family == 0U ? 50U : (family == 1U ? 60U : 45U)) + shade / 3U;
            }
            row[index] = Rgb565(r * scale / 255U, g * scale / 255U, b * scale / 255U);
        }
    }
}

// A closed 8 x 4 x 8 box the camera sits inside; every wall is split into
// 1-unit quads so the MeshRenderer has realistic face counts to sort.
class Room final {
   public:
    static constexpr int kSize = 8;
    static constexpr int kHeight = 4;
    static constexpr uint32_t kVertexCount = 4U * ((kSize + 1) * (kHeight + 1)) + 2U * ((kSize + 1) * (kSize + 1));
    static constexpr uint32_t kFaceCount = 4U * kSize * kHeight + 2U * kSize * kSize;

    Room() {
        uint32_t v = 0U;
        uint32_t f = 0U;
        const float half = static_cast<float>(kSize) * 0.5F;
        // Walls: each wall is a grid of (kSize+1) x (kHeight+1) vertices.
        for (int wall = 0; wall < 4; ++wall) {
            const uint32_t base = v;
            for (int j = 0; j <= kHeight; ++j) {
                for (int i = 0; i <= kSize; ++i) {
                    const float a = static_cast<float>(i) - half;
                    const float y = static_cast<float>(j) - 1.0F;
                    micropixel::MeshVertex p{};
                    switch (wall) {
                        case 0:  // far wall (z = +half), seen from -z: x runs right
                            p = {a, y, half};
                            break;
                        case 1:  // right wall (x = +half), seen from -x: z runs from far to near
                            p = {half, y, half - static_cast<float>(i)};
                            break;
                        case 2:  // near wall (z = -half)
                            p = {-a, y, -half};
                            break;
                        default:  // left wall (x = -half)
                            p = {-half, y, -half + static_cast<float>(i)};
                            break;
                    }
                    vertices_[v++] = p;
                }
            }
            for (int j = 0; j < kHeight; ++j) {
                for (int i = 0; i < kSize; ++i) {
                    // `i` runs to the right as seen from inside the room, so
                    // bottom-left, bottom-right, top-right, top-left is
                    // counter-clockwise from the front.
                    const auto at = [&](int ii, int jj) { return static_cast<uint16_t>(base + jj * (kSize + 1) + ii); };
                    micropixel::MeshFace& face = faces_[f++];
                    face.vertex[0] = at(i, j);
                    face.vertex[1] = at(i + 1, j);
                    face.vertex[2] = at(i + 1, j + 1);
                    face.vertex[3] = at(i, j + 1);
                    SetTexels(face, i, j);
                }
            }
        }
        // Floor (y = -1) and ceiling (y = kHeight - 1).
        for (int plane = 0; plane < 2; ++plane) {
            const uint32_t base = v;
            const float y = plane == 0 ? -1.0F : static_cast<float>(kHeight) - 1.0F;
            for (int j = 0; j <= kSize; ++j) {
                for (int i = 0; i <= kSize; ++i) {
                    vertices_[v++] = {static_cast<float>(i) - half, y, static_cast<float>(j) - half};
                }
            }
            for (int j = 0; j < kSize; ++j) {
                for (int i = 0; i < kSize; ++i) {
                    const auto at = [&](int ii, int jj) { return static_cast<uint16_t>(base + jj * (kSize + 1) + ii); };
                    micropixel::MeshFace& face = faces_[f++];
                    if (plane == 0) {  // floor faces up
                        face.vertex[0] = at(i, j);
                        face.vertex[1] = at(i + 1, j);
                        face.vertex[2] = at(i + 1, j + 1);
                        face.vertex[3] = at(i, j + 1);
                    } else {  // ceiling faces down
                        face.vertex[0] = at(i, j + 1);
                        face.vertex[1] = at(i + 1, j + 1);
                        face.vertex[2] = at(i + 1, j);
                        face.vertex[3] = at(i, j);
                    }
                    SetTexels(face, i, j);
                }
            }
        }
    }

    [[nodiscard]] micropixel::Mesh mesh() const {
        return {std::span<const micropixel::MeshVertex>(vertices_, kVertexCount),
                std::span<const micropixel::MeshFace>(faces_, kFaceCount)};
    }

   private:
    static void SetTexels(micropixel::MeshFace& face, int i, int j) {
        const uint16_t u0 = static_cast<uint16_t>((i & 3) * kTextureSize);
        const uint16_t v0 = static_cast<uint16_t>((j & 3) * kTextureSize);
        // Vertex order is (i,j) (i+1,j) (i+1,j+1) (i,j+1) (the ceiling lists it
        // reversed, which only mirrors its texture).
        const uint16_t us[4] = {u0, static_cast<uint16_t>(u0 + kTextureSize), static_cast<uint16_t>(u0 + kTextureSize),
                                u0};
        const uint16_t vs[4] = {static_cast<uint16_t>(v0 + kTextureSize), static_cast<uint16_t>(v0 + kTextureSize), v0,
                                v0};
        for (int c = 0; c < 4; ++c) {
            face.u[c] = us[c];
            face.v[c] = vs[c];
            face.brightness[c] = static_cast<uint8_t>(160U + ((i * 37 + j * 11 + c * 23) % 96));
        }
        face.texture_slot = kTextureSlot;
    }

    micropixel::MeshVertex vertices_[kVertexCount]{};
    micropixel::MeshFace faces_[kFaceCount]{};
};

class Benchmark final {
   public:
    explicit Benchmark(micropixel::Application& app) : app_(app) {}

    int Run() {
        options_ = ParseOptions(app_.launch_arguments());
        const micropixel::RendererInfo display = app_.renderer().info();
        if (!display.polygon_supported()) {
            app_.log().Error("polygon-benchmark: Host has no polygon raster records (Graphics 1.6 polygon cap)");
            return 1;
        }
        auto created = app_.renderer().CreateHostSurface(kBufferCount, options_.upscale);
        if (!created.has_value()) {
            app_.log().Error("polygon-benchmark: HostSurface unavailable");
            return 1;
        }
        surface_ = static_cast<micropixel::HostSurface&&>(created.value());
        auto raster = app_.renderer().CreateRasterResources();
        if (!raster.has_value()) {
            app_.log().Error("polygon-benchmark: raster resources unavailable");
            return 1;
        }
        raster_ = raster.value();
        width_ = static_cast<int>(surface_.buffer_width());
        height_ = static_cast<int>(surface_.buffer_height());

        BuildTexture(texels_);
        BuildPalette(palette_);
        if (!raster_
                 .UploadTexture(kTextureSlot, kTextureSize, kTextureSize, micropixel::RasterLayout::kRowMajor,
                                std::span<const uint8_t>(texels_, sizeof(texels_)))
                 .has_value() ||
            !raster_
                 .UploadLitPalette(kPaletteSlot, kLightLevels, std::span<const uint16_t>(palette_, kLightLevels * 256U))
                 .has_value()) {
            app_.log().Error("polygon-benchmark: texture or palette upload rejected");
            return 2;
        }

        micropixel::MeshRendererConfig config{};
        config.width = width_;
        config.height = height_;
        config.lighting.levels = kLightLevels;
        config.lighting.minimum = 1U;
        config.lighting.full_distance = 2.0F;
        config.lighting.dark_distance = 14.0F;
        config.far = 32.0F;
        config.subdivide_min_pixels = 96;
        config.subdivide_levels = 1U;
        if (!mesh_.Initialize(config, pool_.storage(), pool_.groups())) {
            app_.log().Error("polygon-benchmark: MeshRenderer initialise failed");
            return 2;
        }

        {
            Line msg;
            msg.Append("polygon-benchmark: ");
            msg.AppendUint(surface_.buffer_width());
            msg.Append("x");
            msg.AppendUint(surface_.buffer_height());
            msg.Append(" upscale=");
            msg.AppendUint(options_.upscale);
            msg.Append(surface_.direct_scanout() ? ", direct scanout" : ", composited fallback");
            msg.Append(", panel max ");
            msg.AppendUint(surface_.max_full_frame_fps());
            msg.Append(" fps, room faces=");
            msg.AppendUint(Room::kFaceCount);
            app_.log().Info(msg.c_str());
        }

        window_start_us_ = app_.clock().Now().microseconds();
        for (;;) {
            if (!DrainEvents()) break;
            uint32_t index = 0U;
            const uint64_t wait_started_us = app_.clock().Now().microseconds();
            while (!surface_.AcquireFree(index)) {
                if (!HandleEvent(app_.WaitEvent())) return 0;
            }
            const uint64_t render_started_us = app_.clock().Now().microseconds();
            wait_us_ += render_started_us - wait_started_us;

            const Phase phase = CurrentPhase();
            bool drawn = false;
            uint32_t pixels = 0U;
            uint32_t polygons = 0U;
            auto rendered = surface_.Update(
                index, [&](micropixel::RasterDrawList& list) { drawn = Draw(phase, list, pixels, polygons); });
            if (!drawn || !rendered.has_value()) {
                app_.log().Error("polygon-benchmark: Host raster rejected the frame's records");
                return 4;
            }
            const uint64_t render_done_us = app_.clock().Now().microseconds();
            if (!surface_.Present(index).has_value()) {
                app_.log().Error("polygon-benchmark: SURFACE_PRESENT rejected");
                return 3;
            }
            const uint64_t presented_us = app_.clock().Now().microseconds();
            render_us_ += render_done_us - render_started_us;
            present_us_ += presented_us - render_done_us;
            pixels_ += pixels;
            polygons_ += polygons;
            ++frames_;
            ++frame_index_;
            if (frames_ == kStatsWindow) {
                LogWindow(phase, presented_us);
            }
        }
        surface_.Reset();
        return 0;
    }

   private:
    Phase CurrentPhase() const {
        if (options_.fixed_phase >= 0) return static_cast<Phase>(options_.fixed_phase);
        return static_cast<Phase>((frame_index_ / options_.frames_per_phase) % static_cast<uint32_t>(Phase::kCount));
    }

    bool DrainEvents() {
        micropixel::Event event;
        while (app_.PollEvent(event)) {
            if (!HandleEvent(event)) return false;
        }
        return true;
    }

    bool HandleEvent(const micropixel::Event& event) {
        // Pause returns every buffer; AcquireFree() simply succeeds again on resume.
        return event.type() != micropixel::EventType::kStop;
    }

    // Random convex textured quads whose total area is `overdraw` buffers.
    bool DrawRandomQuads(micropixel::RasterDrawList& list, int extent, float overdraw, uint32_t& pixels,
                         uint32_t& polygons) {
        Rng rng(frame_index_ * 2654435761U + 17U);
        const uint32_t area = static_cast<uint32_t>(width_) * static_cast<uint32_t>(height_);
        const uint32_t target = static_cast<uint32_t>(static_cast<float>(area) * overdraw);
        // Each quad is a jittered square: corners pulled in by up to a quarter
        // of the extent keep it convex; the area is roughly extent^2 * 0.75.
        const uint32_t quad_area = static_cast<uint32_t>(extent) * static_cast<uint32_t>(extent) * 3U / 4U;
        uint32_t count = target / quad_area;
        if (count > kMaxQuads) count = kMaxQuads;
        const int jitter = extent / 4;
        for (uint32_t q = 0U; q < count; ++q) {
            const int x = rng.Range(0, width_ - extent);
            const int y = rng.Range(0, height_ - extent);
            const uint8_t light = static_cast<uint8_t>(rng.Range(6, kLightLevels - 1));
            const uint8_t light2 = static_cast<uint8_t>(rng.Range(2, kLightLevels - 1));
            const float e = static_cast<float>(extent);
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            const float j0 = static_cast<float>(rng.Range(0, jitter));
            const float j1 = static_cast<float>(rng.Range(0, jitter));
            const float j2 = static_cast<float>(rng.Range(0, jitter));
            const float j3 = static_cast<float>(rng.Range(0, jitter));
            const micropixel::RasterVertex corners[4] = {
                micropixel::RasterVertex::At(fx + j0, fy + j1, 0.0F, 0.0F, light),
                micropixel::RasterVertex::At(fx + e - j1, fy + j2, 64.0F, 0.0F, light2),
                micropixel::RasterVertex::At(fx + e - j2, fy + e - j3, 64.0F, 64.0F, light),
                micropixel::RasterVertex::At(fx + j3, fy + e - j0, 0.0F, 64.0F, light2),
            };
            if (!list.Quad(corners, kTextureSlot)) return false;
            // Shoelace area of the actual quad.
            float area2 = 0.0F;
            for (int c = 0; c < 4; ++c) {
                const micropixel::RasterVertex& a = corners[c];
                const micropixel::RasterVertex& b = corners[(c + 1) & 3];
                area2 += static_cast<float>(a.x) * static_cast<float>(b.y) -
                         static_cast<float>(b.x) * static_cast<float>(a.y);
            }
            const float area_px = (area2 < 0.0F ? -area2 : area2) * 0.5F / 256.0F;
            pixels += static_cast<uint32_t>(area_px);
        }
        polygons += count;
        return true;
    }

    bool DrawRoom(micropixel::RasterDrawList& list, uint32_t& pixels, uint32_t& polygons) {
        micropixel::MeshCamera camera{};
        camera.position = {0.0F, 0.6F, -1.5F};
        camera.yaw = static_cast<float>(frame_index_ % 720U) * (6.2831853F / 720.0F);
        camera.pitch = 0.0F;
        camera.focal_length = micropixel::MeshCamera::FocalLength(1.5707963F, width_);
        camera.near = 0.1F;
        mesh_.Begin(camera);
        if (!mesh_.Submit(room_.mesh(), micropixel::Transform3::Identity())) return false;
        if (!mesh_.Flush(list)) return false;
        pixels += mesh_.stats().pixel_estimate;
        polygons += mesh_.stats().polygons;
        room_dropped_ += mesh_.stats().dropped;
        room_subdivided_ += mesh_.stats().subdivided;
        return true;
    }

    bool Draw(Phase phase, micropixel::RasterDrawList& list, uint32_t& pixels, uint32_t& polygons) {
        list.SetPalette(kPaletteSlot);
        switch (phase) {
            case Phase::kFill: {
                const float w = static_cast<float>(width_);
                const float h = static_cast<float>(height_);
                const uint8_t light = static_cast<uint8_t>(8U + (frame_index_ / 8U) % 8U);
                const micropixel::RasterVertex corners[4] = {
                    micropixel::RasterVertex::At(0.0F, 0.0F, 0.0F, 0.0F, light),
                    micropixel::RasterVertex::At(w, 0.0F, 256.0F, 0.0F, light),
                    micropixel::RasterVertex::At(w, h, 256.0F, 256.0F, light),
                    micropixel::RasterVertex::At(0.0F, h, 0.0F, 256.0F, light),
                };
                pixels += static_cast<uint32_t>(width_) * static_cast<uint32_t>(height_);
                polygons += 1U;
                return list.Quad(corners, kTextureSlot);
            }
            case Phase::kQuads1x:
                return DrawRandomQuads(list, 120, 1.0F, pixels, polygons);
            case Phase::kQuads15x:
                return DrawRandomQuads(list, 120, 1.5F, pixels, polygons);
            case Phase::kQuads2x:
                return DrawRandomQuads(list, 120, 2.0F, pixels, polygons);
            case Phase::kSmall2x:
                return DrawRandomQuads(list, 24, 2.0F, pixels, polygons);
            case Phase::kRoom:
                // Clear to black first so the area outside the room does not
                // keep stale pixels (a real game always covers the buffer).
                if (!list.FillRect({0, 0, width_, height_}, micropixel::Color::Rgb(0U, 0U, 0U))) return false;
                pixels += static_cast<uint32_t>(width_) * static_cast<uint32_t>(height_);
                return DrawRoom(list, pixels, polygons);
            default:
                return false;
        }
    }

    void LogWindow(Phase phase, uint64_t now_us) {
        const uint64_t elapsed_us = now_us - window_start_us_;
        Line msg;
        msg.Append("polygon-bench: phase=");
        msg.Append(PhaseName(phase));
        msg.Append(" frames=");
        msg.AppendUint(frames_);
        msg.Append(" fps_x100=");
        msg.AppendUint(elapsed_us == 0U ? 0U : frames_ * 100000000ULL / elapsed_us);
        msg.Append(" render_avg_us=");
        msg.AppendUint(render_us_ / frames_);
        msg.Append(" present_avg_us=");
        msg.AppendUint(present_us_ / frames_);
        msg.Append(" wait_avg_us=");
        msg.AppendUint(wait_us_ / frames_);
        msg.Append(" polygons=");
        msg.AppendUint(polygons_ / frames_);
        msg.Append(" pixels=");
        msg.AppendUint(pixels_ / frames_);
        msg.Append(" overdraw_x100=");
        msg.AppendUint(pixels_ * 100ULL / frames_ / (static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_)));
        msg.Append(" ns_per_pixel=");
        msg.AppendUint(pixels_ == 0U ? 0U : render_us_ * 1000ULL / pixels_);
        if (phase == Phase::kRoom) {
            msg.Append(" subdivided=");
            msg.AppendUint(room_subdivided_ / frames_);
            msg.Append(" dropped=");
            msg.AppendUint(room_dropped_);
        }
        app_.log().Info(msg.c_str());
        frames_ = 0U;
        render_us_ = 0U;
        present_us_ = 0U;
        wait_us_ = 0U;
        pixels_ = 0U;
        polygons_ = 0U;
        room_dropped_ = 0U;
        room_subdivided_ = 0U;
        window_start_us_ = now_us;
    }

    micropixel::Application& app_;
    Options options_{};
    micropixel::HostSurface surface_{};
    micropixel::RasterResources raster_{};
    int width_{};
    int height_{};
    uint8_t texels_[kTextureSize * kTextureSize]{};
    uint16_t palette_[kLightLevels * 256U]{};
    Room room_{};
    micropixel::MeshRendererPool<1024> pool_{};
    micropixel::MeshRenderer mesh_{};
    uint32_t frame_index_{};
    uint32_t frames_{};
    uint64_t render_us_{};
    uint64_t present_us_{};
    uint64_t wait_us_{};
    uint64_t pixels_{};
    uint64_t polygons_{};
    uint32_t room_dropped_{};
    uint32_t room_subdivided_{};
    uint64_t window_start_us_{};
};

}  // namespace

int main() {
    micropixel::Application app;
    // The benchmark state (texture, palette, room, polygon pool) is well over
    // the WAMR call stack budget; keep it on the heap.
    auto benchmark = std::make_unique<Benchmark>(app);
    return benchmark->Run();
}
