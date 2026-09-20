#ifndef MICROPIXEL_SDK_GAMEPAD_SKIN_HPP
#define MICROPIXEL_SDK_GAMEPAD_SKIN_HPP

#include <stdint.h>

#include <memory>
#include <span>

#include "sdk/gamepad.hpp"
#include "sdk/graphics.hpp"
#include "sdk/resources.hpp"
#include "sdk/scene.hpp"

namespace micropixel {

// Colours of the default on-screen gamepad. Opacity is straight alpha baked
// into the atlas; `Draw`/`Sync` apply `overlay_opacity` on top. Idle controls
// use faint matching rims, clear interiors and neutral glyphs; presses add a subtle dark grey fill.
struct GamepadSkinStyle final {
    Color ring{GamepadButtonStyle{}.rim};
    uint8_t ring_opacity{GamepadButtonStyle{}.rim_opacity};
    Color knob{GamepadButtonStyle{}.pressed_fill};
    uint8_t knob_opacity{128U};
    uint8_t overlay_opacity{192U};
    // Floating sticks normally appear only while engaged; fixed sticks stay visible.
    bool show_stick_at_rest{false};
};

namespace detail {

// Glyphs are small anti-aliased vector drawings (discs, strokes, triangles)
// in a unit square, so they scale with the button and match smooth game art
// better than scaled pixel bitmaps.
struct GlyphPoint final {
    float x{};
    float y{};
};

// Straight-alpha BGRA8888 canvas used while baking the atlas.
class GamepadAtlasCanvas final {
   public:
    GamepadAtlasCanvas(uint8_t* pixels, int32_t width, int32_t height)
        : pixels_(pixels), width_(width), height_(height) {}

    [[nodiscard]] constexpr uint32_t pitch() const { return static_cast<uint32_t>(width_) * 4U; }

    // Composites `color` at `alpha` over the pixel ("over" operator).
    void Blend(int32_t x, int32_t y, Color color, uint32_t alpha) {
        if (alpha == 0U || x < 0 || y < 0 || x >= width_ || y >= height_) {
            return;
        }
        uint8_t* pixel =
            pixels_ + (static_cast<uint32_t>(y) * static_cast<uint32_t>(width_) + static_cast<uint32_t>(x)) * 4U;
        const uint32_t dst_alpha = pixel[3];
        const uint32_t out_alpha = alpha + dst_alpha * (255U - alpha) / 255U;
        if (out_alpha == 0U) {
            return;
        }
        const uint32_t src_weight = alpha * 255U;
        const uint32_t dst_weight = dst_alpha * (255U - alpha);
        const uint32_t total = src_weight + dst_weight;
        pixel[0] = static_cast<uint8_t>((color.blue() * src_weight + pixel[0] * dst_weight) / total);
        pixel[1] = static_cast<uint8_t>((color.green() * src_weight + pixel[1] * dst_weight) / total);
        pixel[2] = static_cast<uint8_t>((color.red() * src_weight + pixel[2] * dst_weight) / total);
        pixel[3] = static_cast<uint8_t>(out_alpha);
    }

    // Anti-aliased disc; `inner_radius` > 0 leaves a hole (ring).
    void Disc(Point centre, float radius, float inner_radius, Color color, uint8_t opacity) {
        const int32_t extent = static_cast<int32_t>(radius) + 2;
        for (int32_t y = centre.y - extent; y <= centre.y + extent; ++y) {
            for (int32_t x = centre.x - extent; x <= centre.x + extent; ++x) {
                const float dx = static_cast<float>(x - centre.x);
                const float dy = static_cast<float>(y - centre.y);
                const float distance = math::Sqrt(dx * dx + dy * dy);
                float coverage = math::Clamp(radius + 0.5F - distance, 0.0F, 1.0F);
                if (inner_radius > 0.0F) {
                    coverage -= math::Clamp(inner_radius + 0.5F - distance, 0.0F, 1.0F);
                }
                if (coverage <= 0.0F) {
                    continue;
                }
                Blend(x, y, color, static_cast<uint32_t>(coverage * static_cast<float>(opacity) + 0.5F));
            }
        }
    }

    // Anti-aliased shapes from signed distance; `clip` (x, y in canvas pixels)
    // may reject pixels, which is how half rings are cut.
    template <typename Distance, typename Clip>
    void Shape(int32_t left, int32_t top, int32_t right, int32_t bottom, Color color, Distance&& distance,
               Clip&& clip) {
        for (int32_t y = top; y <= bottom; ++y) {
            for (int32_t x = left; x <= right; ++x) {
                if (!clip(x, y)) {
                    continue;
                }
                const float coverage =
                    math::Clamp(0.5F - distance(static_cast<float>(x), static_cast<float>(y)), 0.0F, 1.0F);
                if (coverage > 0.0F) {
                    Blend(x, y, color, static_cast<uint32_t>(coverage * 255.0F + 0.5F));
                }
            }
        }
    }

    void Dot(float cx, float cy, float radius, Color color) {
        const int32_t extent = static_cast<int32_t>(radius) + 2;
        Shape(
            static_cast<int32_t>(cx) - extent, static_cast<int32_t>(cy) - extent, static_cast<int32_t>(cx) + extent,
            static_cast<int32_t>(cy) + extent, color,
            [&](float x, float y) { return math::Sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - radius; },
            [](int32_t, int32_t) { return true; });
    }

    // Ring of `thickness`; `half` > 0 keeps only x >= cx, < 0 only x <= cx.
    void Hoop(float cx, float cy, float radius, float thickness, Color color, int half = 0) {
        const int32_t extent = static_cast<int32_t>(radius + thickness) + 2;
        Shape(
            static_cast<int32_t>(cx) - extent, static_cast<int32_t>(cy) - extent, static_cast<int32_t>(cx) + extent,
            static_cast<int32_t>(cy) + extent, color,
            [&](float x, float y) {
                return math::Abs(math::Sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) - radius) - thickness * 0.5F;
            },
            [&](int32_t x, int32_t) {
                return half == 0 || (half > 0 ? static_cast<float>(x) >= cx : static_cast<float>(x) <= cx);
            });
    }

    // Rounded stroke between two points.
    void Stroke(GlyphPoint a, GlyphPoint b, float width, Color color) {
        const float left = math::Min(a.x, b.x) - width;
        const float top = math::Min(a.y, b.y) - width;
        const float right = math::Max(a.x, b.x) + width;
        const float bottom = math::Max(a.y, b.y) + width;
        Shape(
            static_cast<int32_t>(left) - 1, static_cast<int32_t>(top) - 1, static_cast<int32_t>(right) + 1,
            static_cast<int32_t>(bottom) + 1, color,
            [&](float x, float y) {
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float length2 = dx * dx + dy * dy;
                const float t =
                    length2 > 0.0F ? math::Clamp(((x - a.x) * dx + (y - a.y) * dy) / length2, 0.0F, 1.0F) : 0.0F;
                const float px = a.x + dx * t - x;
                const float py = a.y + dy * t - y;
                return math::Sqrt(px * px + py * py) - width * 0.5F;
            },
            [](int32_t, int32_t) { return true; });
    }

    // Filled triangle in either winding.
    void Triangle(GlyphPoint a, GlyphPoint b, GlyphPoint c, Color color) {
        const auto edge = [](GlyphPoint p, GlyphPoint from, GlyphPoint to) {
            const float dx = to.x - from.x;
            const float dy = to.y - from.y;
            const float length2 = dx * dx + dy * dy;
            const float t =
                length2 > 0.0F ? math::Clamp(((p.x - from.x) * dx + (p.y - from.y) * dy) / length2, 0.0F, 1.0F) : 0.0F;
            const float px = from.x + dx * t - p.x;
            const float py = from.y + dy * t - p.y;
            return math::Sqrt(px * px + py * py);
        };
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        const float sign = area >= 0.0F ? 1.0F : -1.0F;
        const auto inside = [&](GlyphPoint p) {
            const float s0 = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) * sign;
            const float s1 = ((c.x - b.x) * (p.y - b.y) - (p.x - b.x) * (c.y - b.y)) * sign;
            const float s2 = ((a.x - c.x) * (p.y - c.y) - (p.x - c.x) * (a.y - c.y)) * sign;
            return s0 >= 0.0F && s1 >= 0.0F && s2 >= 0.0F;
        };
        const float left = math::Min(a.x, math::Min(b.x, c.x));
        const float top = math::Min(a.y, math::Min(b.y, c.y));
        const float right = math::Max(a.x, math::Max(b.x, c.x));
        const float bottom = math::Max(a.y, math::Max(b.y, c.y));
        Shape(
            static_cast<int32_t>(left) - 1, static_cast<int32_t>(top) - 1, static_cast<int32_t>(right) + 1,
            static_cast<int32_t>(bottom) + 1, color,
            [&](float x, float y) {
                const GlyphPoint p{x, y};
                const float d = math::Min(edge(p, a, b), math::Min(edge(p, b, c), edge(p, c, a)));
                return inside(p) ? -d : d;
            },
            [](int32_t, int32_t) { return true; });
    }

    // Draws `glyph` centred at `centre`; `half` is half the glyph's extent in
    // pixels (the unit square maps to [-half, half]). `cut` is the button fill,
    // used to carve details (finger gaps) out of solid silhouettes.
    void Glyph(Point centre, GamepadGlyph glyph, float half, Color color, Color cut) {
        const float cx = static_cast<float>(centre.x);
        const float cy = static_cast<float>(centre.y);
        const auto at = [&](float ux, float uy) { return GlyphPoint{cx + ux * half, cy + uy * half}; };
        const float stroke = math::Max(half * 0.22F, 1.5F);
        const float thin = math::Max(half * 0.16F, 1.2F);
        switch (glyph) {
            case GamepadGlyph::kNone:
                return;
            case GamepadGlyph::kA:
                Stroke(at(-0.75F, 0.9F), at(0.0F, -0.9F), stroke, color);
                Stroke(at(0.75F, 0.9F), at(0.0F, -0.9F), stroke, color);
                Stroke(at(-0.42F, 0.3F), at(0.42F, 0.3F), stroke, color);
                return;
            case GamepadGlyph::kB:
                Stroke(at(-0.55F, -0.9F), at(-0.55F, 0.9F), stroke, color);
                Stroke(at(-0.55F, -0.9F), at(0.0F, -0.9F), stroke, color);
                Stroke(at(-0.55F, 0.0F), at(0.05F, 0.0F), stroke, color);
                Stroke(at(-0.55F, 0.9F), at(0.05F, 0.9F), stroke, color);
                Hoop(cx + 0.0F * half, cy - 0.45F * half, 0.45F * half, stroke, color, 1);
                Hoop(cx + 0.05F * half, cy + 0.45F * half, 0.45F * half, stroke, color, 1);
                return;
            case GamepadGlyph::kX:
                Stroke(at(-0.75F, -0.85F), at(0.75F, 0.85F), stroke, color);
                Stroke(at(0.75F, -0.85F), at(-0.75F, 0.85F), stroke, color);
                return;
            case GamepadGlyph::kY:
                Stroke(at(-0.75F, -0.85F), at(0.0F, 0.0F), stroke, color);
                Stroke(at(0.75F, -0.85F), at(0.0F, 0.0F), stroke, color);
                Stroke(at(0.0F, 0.0F), at(0.0F, 0.9F), stroke, color);
                return;
            case GamepadGlyph::kJump:
                // Arrow leaving a ground line.
                Stroke(at(0.0F, -0.35F), at(0.0F, 0.45F), stroke, color);
                Triangle(at(-0.6F, -0.2F), at(0.6F, -0.2F), at(0.0F, -0.95F), color);
                Stroke(at(-0.8F, 0.85F), at(0.8F, 0.85F), thin, color);
                return;
            case GamepadGlyph::kFire:
                // Crosshair: ring, four ticks, centre dot.
                Hoop(cx, cy, 0.55F * half, thin, color);
                Stroke(at(0.0F, -0.95F), at(0.0F, -0.6F), thin, color);
                Stroke(at(0.0F, 0.6F), at(0.0F, 0.95F), thin, color);
                Stroke(at(-0.95F, 0.0F), at(-0.6F, 0.0F), thin, color);
                Stroke(at(0.6F, 0.0F), at(0.95F, 0.0F), thin, color);
                Dot(cx, cy, math::Max(half * 0.14F, 1.5F), color);
                return;
            case GamepadGlyph::kPunch:
                // Fist seen from the front: four fingers curled over the palm,
                // thumb tucked along the side, gaps carved in the button colour.
                Stroke(at(-0.5F, 0.3F), at(0.55F, 0.3F), half * 1.0F, color);
                Dot(cx - 0.62F * half, cy - 0.3F * half, half * 0.3F, color);
                Dot(cx - 0.2F * half, cy - 0.4F * half, half * 0.3F, color);
                Dot(cx + 0.22F * half, cy - 0.4F * half, half * 0.3F, color);
                Dot(cx + 0.64F * half, cy - 0.3F * half, half * 0.3F, color);
                Stroke(at(-0.41F, -0.3F), at(-0.41F, 0.1F), thin * 0.6F, cut);
                Stroke(at(0.01F, -0.4F), at(0.01F, 0.1F), thin * 0.6F, cut);
                Stroke(at(0.43F, -0.3F), at(0.43F, 0.1F), thin * 0.6F, cut);
                Stroke(at(-0.95F, 0.05F), at(-0.7F, 0.6F), half * 0.4F, color);
                Stroke(at(-0.62F, 0.15F), at(-0.5F, 0.62F), thin * 0.6F, cut);
                return;
            case GamepadGlyph::kSword:
                // Blade with a pointed tip, cross guard and grip on a diagonal.
                Stroke(at(0.72F, -0.72F), at(-0.15F, 0.15F), stroke, color);
                Triangle(at(0.83F, -0.61F), at(0.61F, -0.83F), at(1.0F, -1.0F), color);
                Stroke(at(-0.5F, -0.2F), at(0.2F, 0.5F), thin, color);
                Stroke(at(-0.3F, 0.3F), at(-0.85F, 0.85F), stroke, color);
                return;
            case GamepadGlyph::kShield:
                // Rounded shield: top bar, sides and a pointed bottom.
                Stroke(at(-0.7F, -0.75F), at(0.7F, -0.75F), thin, color);
                Stroke(at(-0.7F, -0.75F), at(-0.7F, 0.1F), thin, color);
                Stroke(at(0.7F, -0.75F), at(0.7F, 0.1F), thin, color);
                Stroke(at(-0.7F, 0.1F), at(0.0F, 0.9F), thin, color);
                Stroke(at(0.7F, 0.1F), at(0.0F, 0.9F), thin, color);
                Stroke(at(0.0F, -0.45F), at(0.0F, 0.45F), thin, color);
                return;
            case GamepadGlyph::kRun:
                // Lightning bolt.
                Triangle(at(0.15F, -0.95F), at(-0.6F, 0.1F), at(0.1F, 0.1F), color);
                Triangle(at(0.1F, -0.15F), at(0.6F, -0.15F), at(-0.2F, 0.95F), color);
                return;
            case GamepadGlyph::kInteract:
                // Speech dots.
                Dot(cx - 0.55F * half, cy, half * 0.18F, color);
                Dot(cx, cy, half * 0.18F, color);
                Dot(cx + 0.55F * half, cy, half * 0.18F, color);
                return;
            case GamepadGlyph::kPause:
                Stroke(at(-0.35F, -0.7F), at(-0.35F, 0.7F), stroke * 1.3F, color);
                Stroke(at(0.35F, -0.7F), at(0.35F, 0.7F), stroke * 1.3F, color);
                return;
            case GamepadGlyph::kMenu:
                Stroke(at(-0.75F, -0.55F), at(0.75F, -0.55F), thin, color);
                Stroke(at(-0.75F, 0.0F), at(0.75F, 0.0F), thin, color);
                Stroke(at(-0.75F, 0.55F), at(0.75F, 0.55F), thin, color);
                return;
            case GamepadGlyph::kArrowUp:
                Stroke(at(0.0F, -0.2F), at(0.0F, 0.85F), stroke, color);
                Triangle(at(-0.7F, -0.1F), at(0.7F, -0.1F), at(0.0F, -0.9F), color);
                return;
            case GamepadGlyph::kArrowDown:
                Stroke(at(0.0F, -0.85F), at(0.0F, 0.2F), stroke, color);
                Triangle(at(-0.7F, 0.1F), at(0.7F, 0.1F), at(0.0F, 0.9F), color);
                return;
            case GamepadGlyph::kArrowLeft:
                Stroke(at(-0.2F, 0.0F), at(0.85F, 0.0F), stroke, color);
                Triangle(at(-0.1F, -0.7F), at(-0.1F, 0.7F), at(-0.9F, 0.0F), color);
                return;
            case GamepadGlyph::kArrowRight:
                Stroke(at(-0.85F, 0.0F), at(0.2F, 0.0F), stroke, color);
                Triangle(at(0.1F, -0.7F), at(0.1F, 0.7F), at(0.9F, 0.0F), color);
                return;
        }
    }

   private:
    uint8_t* pixels_;
    int32_t width_;
    int32_t height_;
};

}  // namespace detail

// Default look of a VirtualGamepad: a translucent ring and knob for the stick
// and round buttons with preset glyphs, baked once into a dynamic texture so
// each frame costs a handful of Image records (HostSurface) or SpriteBatch
// instances (Scene). Games with their own art can skip this class and draw
// from VirtualGamepad::stick_geometry() / button_geometry() instead.
//
//   skin.Initialize(app.resources(), pad);
//   surface.Update(index, [&](RasterDrawList& list) { ...; (void)skin.Draw(list, pad); });
//   // or, for Scene Apps:
//   (void)skin.Attach(scene);  ...  skin.Sync(pad);  renderer.Present(scene);
class GamepadSkin final {
   public:
    GamepadSkin() = default;
    GamepadSkin(const GamepadSkin&) = delete;
    GamepadSkin& operator=(const GamepadSkin&) = delete;
    GamepadSkin(GamepadSkin&&) noexcept = default;
    GamepadSkin& operator=(GamepadSkin&&) noexcept = default;

    // Bakes the atlas for the pad's current geometry and glyphs. Call again
    // after reconfiguring the pad; false when the pad is unconfigured or the
    // Host refused the texture (the game keeps working without an overlay).
    bool Initialize(Resources resources, const VirtualGamepad& pad, const GamepadSkinStyle& style = {}) {
        Reset();
        if (!pad.configured()) {
            return false;
        }
        style_ = style;
        ring_radius_ = pad.stick_radius();
        knob_radius_ = math::Max(ring_radius_ * 9 / 25, 4);
        button_count_ = pad.button_count();
        const int32_t ring_tile = ring_radius_ * 2 + 4;
        const int32_t knob_tile = knob_radius_ * 2 + 4;
        int32_t width = ring_tile + knob_tile;
        int32_t height = math::Max(ring_tile, knob_tile);
        for (uint8_t index = 0U; index < button_count_; ++index) {
            const int32_t tile = pad.button_geometry(index).radius * 2 + 4;
            width += tile * 2;
            height = math::Max(height, tile);
        }
        std::unique_ptr<uint8_t[]> pixels(new uint8_t[static_cast<size_t>(width) * static_cast<size_t>(height) * 4U]());
        detail::GamepadAtlasCanvas canvas{pixels.get(), width, height};

        int32_t cursor = 0;
        ring_source_ = {cursor, 0, ring_tile, ring_tile};
        const float ring_thickness = static_cast<float>(math::Max(ring_radius_ / 14, 2));
        canvas.Disc({cursor + ring_tile / 2, ring_tile / 2}, static_cast<float>(ring_radius_),
                    static_cast<float>(ring_radius_) - ring_thickness, style.ring, style.ring_opacity);
        cursor += ring_tile;

        knob_source_ = {cursor, 0, knob_tile, knob_tile};
        canvas.Disc({cursor + knob_tile / 2, knob_tile / 2}, static_cast<float>(knob_radius_), 0.0F, style.knob,
                    style.knob_opacity);
        cursor += knob_tile;

        for (uint8_t index = 0U; index < button_count_; ++index) {
            const VirtualGamepad::ButtonGeometry button = pad.button_geometry(index);
            const GamepadButtonStyle& button_style = pad.buttons()[index].style;
            const int32_t button_tile = button.radius * 2 + 4;
            const float radius = static_cast<float>(button.radius);
            const float glyph_half = radius * 0.5F;
            const float rim = math::Min(ring_thickness, radius);
            for (int pressed = 0; pressed < 2; ++pressed) {
                const Point centre{cursor + button_tile / 2, button_tile / 2};
                if (pressed == 0) {
                    canvas.Disc(centre, radius, 0.0F, button_style.fill, button_style.fill_opacity);
                    canvas.Disc(centre, radius, radius - rim, button_style.rim, button_style.rim_opacity);
                } else {
                    canvas.Disc(centre, radius, 0.0F, button_style.pressed_fill, button_style.pressed_fill_opacity);
                }
                canvas.Glyph(centre, button.glyph, glyph_half, button_style.glyph,
                             pressed == 0 ? button_style.fill : button_style.pressed_fill);
                (pressed == 0 ? button_idle_source_ : button_pressed_source_)[index] = {cursor, 0, button_tile,
                                                                                        button_tile};
                cursor += button_tile;
            }
        }

        const Size size{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
        const std::span<const uint8_t> bytes{pixels.get(), static_cast<size_t>(canvas.pitch()) * size.height};
        auto texture = resources.CreateDynamicTexture(size, PixelFormat::kBgra8888, bytes, canvas.pitch());
        if (!texture.has_value()) {
            return false;
        }
        texture_ = static_cast<Texture&&>(texture.value());
        return true;
    }

    [[nodiscard]] bool valid() const { return texture_.valid(); }
    [[nodiscard]] const Texture& texture() const { return texture_; }

    // Releases the atlas and any Scene batch.
    void Reset() {
        Detach();
        texture_.Reset();
        button_count_ = 0U;
    }

    // HostSurface: appends Image records for the visible controls. `pad`
    // geometry must be in buffer pixels. False when a record was rejected.
    [[nodiscard]] bool Draw(RasterDrawList& list, const VirtualGamepad& pad) const {
        if (!valid() || !pad.overlay_visible()) {
            return true;
        }
        bool ok = true;
        const VirtualGamepad::StickGeometry stick = pad.stick_geometry();
        if (StickVisible(pad, stick)) {
            ok = list.Image(texture_, Centered(stick.origin, ring_source_), ring_source_, style_.overlay_opacity) && ok;
            ok = list.Image(texture_, Centered(stick.knob, knob_source_), knob_source_, style_.overlay_opacity) && ok;
        }
        for (uint8_t index = 0U; index < button_count_; ++index) {
            const VirtualGamepad::ButtonGeometry button = pad.button_geometry(index);
            const Rect& source = button.held ? button_pressed_source_[index] : button_idle_source_[index];
            ok = list.Image(texture_, Centered(button.center, source), source, style_.overlay_opacity) && ok;
        }
        return ok;
    }

    // Scene: creates one SpriteBatch under `parent` for every control. Call
    // Sync() before each Present. False when the batch could not be created.
    [[nodiscard]] bool Attach(Container& parent) {
        Detach();
        if (!valid()) {
            return false;
        }
        auto batch = parent.CreateSpriteBatch(texture_, kBatchCapacity, style_.overlay_opacity);
        if (!batch.has_value()) {
            return false;
        }
        batch_ = batch.value();
        return true;
    }

    void Detach() {
        if (batch_.valid()) {
            batch_.Destroy();
        }
        batch_ = {};
    }

    // Updates the Scene batch from the pad's current geometry.
    void Sync(const VirtualGamepad& pad) {
        if (!batch_.valid()) {
            return;
        }
        const bool visible = pad.overlay_visible();
        const VirtualGamepad::StickGeometry stick = pad.stick_geometry();
        const bool stick_visible = visible && StickVisible(pad, stick);
        SetInstance(0U, stick_visible, Centered(stick.origin, ring_source_), ring_source_);
        SetInstance(1U, stick_visible, Centered(stick.knob, knob_source_), knob_source_);
        for (uint8_t index = 0U; index < GamepadConfig::kMaxButtons; ++index) {
            const bool present = index < button_count_ && index < pad.button_count();
            const VirtualGamepad::ButtonGeometry button = pad.button_geometry(index);
            const Rect& source = button.held ? button_pressed_source_[index] : button_idle_source_[index];
            SetInstance(static_cast<uint16_t>(2U + index), visible && present, Centered(button.center, source), source);
        }
    }

   private:
    static constexpr uint16_t kBatchCapacity = 2U + GamepadConfig::kMaxButtons;

    // A floating stick appears while engaged (finger or keys); a fixed one, or
    // one the style pins, is always drawn.
    [[nodiscard]] bool StickVisible(const VirtualGamepad& pad, const VirtualGamepad::StickGeometry& stick) const {
        return stick.present && (stick.engaged || style_.show_stick_at_rest || !pad.config().floating_stick);
    }

    [[nodiscard]] static Rect Centered(Point centre, const Rect& source) {
        return {centre.x - source.width / 2, centre.y - source.height / 2, source.width, source.height};
    }

    void SetInstance(uint16_t id, bool visible, Rect destination, Rect source) {
        if (!visible) {
            batch_.SetInstanceVisible(id, false);
            return;
        }
        batch_.SetInstance(id, SpriteInstance{.destination = destination, .source = source, .visible = true});
    }

    GamepadSkinStyle style_{};
    Texture texture_{};
    SpriteBatch batch_{};
    int32_t ring_radius_{};
    int32_t knob_radius_{};
    uint8_t button_count_{};
    Rect ring_source_{};
    Rect knob_source_{};
    Rect button_idle_source_[GamepadConfig::kMaxButtons]{};
    Rect button_pressed_source_[GamepadConfig::kMaxButtons]{};
};

}  // namespace micropixel

#endif
