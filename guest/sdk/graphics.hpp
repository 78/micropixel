#ifndef MICROPIXEL_SDK_GRAPHICS_HPP
#define MICROPIXEL_SDK_GRAPHICS_HPP

#include <stdint.h>

#include <span>
#include <type_traits>

#include "sdk/event.hpp"
#include "sdk/geometry.hpp"
#include "sdk/result.hpp"

namespace micropixel {

class Application;
class Renderer;
class Scene;
class Container;
class ContainerNode;
class ShapeNode;
class SpriteNode;
class LabelNode;
class SpriteBatch;
struct SceneDescriptor;
class Texture;
class DirectSurface;
class HostSurface;
class GuestSurface;
// Runtime-internal result of SURFACE_CREATE; defined in the Guest Runtime.
struct DirectSurfaceCreation;
class RasterDrawList;
class Font;

namespace ui {
class ImageButton;
class Label;
class TextButton;
}  // namespace ui

class Color final {
   public:
    [[nodiscard]] static constexpr Color Rgb(uint8_t red, uint8_t green, uint8_t blue) {
        return Color{(static_cast<uint32_t>(red) << 16U) | (static_cast<uint32_t>(green) << 8U) |
                     static_cast<uint32_t>(blue)};
    }

    [[nodiscard]] static constexpr Color Black() { return Rgb(0U, 0U, 0U); }
    [[nodiscard]] static constexpr Color White() { return Rgb(255U, 255U, 255U); }
    [[nodiscard]] static constexpr Color Green() { return Rgb(67U, 214U, 166U); }

    [[nodiscard]] constexpr uint8_t red() const { return static_cast<uint8_t>(rgb888_ >> 16U); }
    [[nodiscard]] constexpr uint8_t green() const { return static_cast<uint8_t>(rgb888_ >> 8U); }
    [[nodiscard]] constexpr uint8_t blue() const { return static_cast<uint8_t>(rgb888_); }
    [[nodiscard]] constexpr uint32_t rgb888() const { return rgb888_; }
    // Canonical RGB565 (5-6-5, red in the high bits), truncating each channel.
    [[nodiscard]] constexpr uint16_t rgb565() const {
        return static_cast<uint16_t>(((rgb888_ >> 8U) & 0xF800U) | ((rgb888_ >> 5U) & 0x07E0U) |
                                     ((rgb888_ >> 3U) & 0x001FU));
    }
    // Widens canonical RGB565 by replicating the top bits into the low ones.
    [[nodiscard]] static constexpr Color FromRgb565(uint16_t value) {
        const uint32_t r5 = value >> 11U;
        const uint32_t g6 = (value >> 5U) & 0x3FU;
        const uint32_t b5 = value & 0x1FU;
        return Rgb(static_cast<uint8_t>((r5 << 3U) | (r5 >> 2U)), static_cast<uint8_t>((g6 << 2U) | (g6 >> 4U)),
                   static_cast<uint8_t>((b5 << 3U) | (b5 >> 2U)));
    }

    [[nodiscard]] static constexpr Color Mix(Color foreground, Color background, uint8_t opacity) {
        const uint32_t inverse = 255U - opacity;
        return Rgb(static_cast<uint8_t>((foreground.red() * opacity + background.red() * inverse + 127U) / 255U),
                   static_cast<uint8_t>((foreground.green() * opacity + background.green() * inverse + 127U) / 255U),
                   static_cast<uint8_t>((foreground.blue() * opacity + background.blue() * inverse + 127U) / 255U));
    }

    [[nodiscard]] constexpr Color Darkened(uint8_t strength) const { return Mix(Black(), *this, strength); }
    [[nodiscard]] constexpr Color Lightened(uint8_t strength) const { return Mix(White(), *this, strength); }

    friend constexpr bool operator==(Color, Color) = default;

   private:
    explicit constexpr Color(uint32_t rgb888) : rgb888_(rgb888) {}
    uint32_t rgb888_{};

    friend class Container;
    friend class ShapeNode;
    friend class LabelNode;
};

struct TextMetrics final {
    uint32_t width{};
    uint32_t height{};
    int32_t baseline{};
};

enum class PixelFormat : uint32_t {
    // Canonical bytes in Guest memory: B, G, R.
    kBgr888 = 1U,
    // Canonical bytes in Guest memory: B, G, R, A.
    kBgra8888 = 2U,
    // Canonical Guest-memory layout: little-endian RGB565 uint16_t.
    kRgb565 = 3U,
};

enum class SystemFont : uint16_t {
    kSmall = 1U,
    kMedium = 2U,
    kLarge = 3U,
    kTitle = 4U,
};

struct DisplayInsets final {
    uint32_t top{};
    uint32_t right{};
    uint32_t bottom{};
    uint32_t left{};
};

class RendererInfo final {
   public:
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    [[nodiscard]] constexpr uint32_t physical_width() const { return physical_width_; }
    [[nodiscard]] constexpr uint32_t physical_height() const { return physical_height_; }
    [[nodiscard]] constexpr DisplayInsets safe_area_insets() const { return safe_area_insets_; }
    [[nodiscard]] constexpr Rect safe_area() const {
        return {static_cast<int32_t>(safe_area_insets_.left), static_cast<int32_t>(safe_area_insets_.top),
                static_cast<int32_t>(width_ - safe_area_insets_.left - safe_area_insets_.right),
                static_cast<int32_t>(height_ - safe_area_insets_.top - safe_area_insets_.bottom)};
    }
    // True when a DirectSurface frame reaches the panel without an App Surface
    // copy. False Hosts still accept DirectSurface but composite every frame.
    [[nodiscard]] constexpr bool direct_scanout() const { return direct_scanout_; }
    // The panel consumes RGB565 with both bytes of every pixel swapped. A
    // GuestSurface App writes its buffers in that order.
    [[nodiscard]] constexpr bool rgb565_byte_swapped() const { return rgb565_byte_swapped_; }
    // Panel transfer bound for one full frame; 0 when the Host does not know.
    [[nodiscard]] constexpr uint16_t max_full_frame_fps() const { return max_full_frame_fps_; }
    // Whether Host raster kernels (RasterResources, HostSurface::Update) are available.
    [[nodiscard]] constexpr bool raster_supported() const { return raster_supported_; }
    // Whether the Host accepts RasterDrawList::Triangle / Quad records.
    [[nodiscard]] constexpr bool polygon_supported() const { return polygon_supported_; }

   private:
    constexpr RendererInfo(uint32_t width, uint32_t height, uint32_t physical_width, uint32_t physical_height,
                           DisplayInsets safe_area_insets, bool direct_scanout, bool rgb565_byte_swapped,
                           uint16_t max_full_frame_fps, bool raster_supported, bool polygon_supported)
        : width_(width),
          height_(height),
          physical_width_(physical_width),
          physical_height_(physical_height),
          safe_area_insets_(safe_area_insets),
          max_full_frame_fps_(max_full_frame_fps),
          direct_scanout_(direct_scanout),
          rgb565_byte_swapped_(rgb565_byte_swapped),
          raster_supported_(raster_supported),
          polygon_supported_(polygon_supported) {}

    uint32_t width_{};
    uint32_t height_{};
    uint32_t physical_width_{};
    uint32_t physical_height_{};
    DisplayInsets safe_area_insets_{};
    uint16_t max_full_frame_fps_{};
    bool direct_scanout_{};
    bool rgb565_byte_swapped_{};
    bool raster_supported_{};
    bool polygon_supported_{};

    friend class Renderer;
};

// Full-screen RGB565 frames scanned out by the Host (Graphics 1.5). The App
// fills one buffer, Presents it and moves on to a free one. A presented buffer
// belongs to the Host until Application delivers the matching
// EventType::kSurfaceReleased, which also updates AcquireFree()/Busy().
//
// A DirectSurface replaces the Scene for as long as it exists: Scene submits
// are rejected while one is created. The Host draws system UI above the frame
// and may composite instead of scanning out while that UI is visible.
//
// This is the frame-pacing part shared by the two concrete surfaces, which
// differ in who owns the pixels:
//   HostSurface   the Host allocates the buffers and the Guest never maps
//                 them; every pixel comes from raster records (Update()).
//   GuestSurface  the SDK allocates the buffers in Guest memory and the App
//                 writes pixels itself (Buffer()).
// Apps hold one of those; a DirectSurface reference only names a surface, for
// example in Event::ReleasedFrom().
class DirectSurface {
   public:
    DirectSurface(const DirectSurface&) = delete;
    DirectSurface& operator=(const DirectSurface&) = delete;

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    // Panel size in physical pixels; every presented frame covers it.
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    // Size of one buffer. Smaller than the panel only when the surface was
    // created with an integer upscale; the Host then enlarges the frame.
    [[nodiscard]] constexpr uint32_t buffer_width() const { return buffer_width_; }
    [[nodiscard]] constexpr uint32_t buffer_height() const { return buffer_height_; }
    [[nodiscard]] constexpr uint32_t buffer_count() const { return buffer_count_; }
    // Byte order of every presented pixel. Host buffers are kept in it; a
    // GuestSurface App writes it (swap the two bytes of each RGB565 value when true).
    [[nodiscard]] constexpr bool rgb565_byte_swapped() const { return rgb565_byte_swapped_; }
    [[nodiscard]] constexpr bool direct_scanout() const { return direct_scanout_; }
    [[nodiscard]] constexpr uint16_t max_full_frame_fps() const { return max_full_frame_fps_; }

    [[nodiscard]] bool Busy(uint32_t index) const;
    // Lowest-numbered buffer the Host does not hold. False when every buffer is
    // in flight; wait for kSurfaceReleased before rendering again.
    [[nodiscard]] bool AcquireFree(uint32_t& index_out) const;
    // Hands `index` to the Host. Fails with kInvalidState when the buffer is
    // still held by the Host.
    [[nodiscard]] Result<void> Present(uint32_t index);
    // Returns every buffer and destroys the surface; Scene submits work again.
    // The concrete surfaces extend it to release what they own.
    void Reset();

   protected:
    constexpr DirectSurface() = default;
    explicit DirectSurface(const DirectSurfaceCreation& creation);
    DirectSurface(DirectSurface&& other) noexcept;
    DirectSurface& operator=(DirectSurface&& other) noexcept;
    // Not deletable through the base: HostSurface/GuestSurface own the lifetime.
    ~DirectSurface();

    // Guest pixel range a present names: buffer `index` starts at
    // `base + index * stride` and spans `length` bytes. All zero for Host
    // buffers, which are named by index alone.
    void SetGuestPixels(uint32_t base, uint32_t stride, uint32_t length) {
        guest_pixels_base_ = base;
        guest_pixels_stride_ = stride;
        guest_pixels_length_ = length;
    }

    uint32_t handle_{};
    uint32_t width_{};
    uint32_t height_{};
    uint32_t buffer_width_{};
    uint32_t buffer_height_{};
    uint32_t buffer_count_{};

   private:
    uint32_t guest_pixels_base_{};
    uint32_t guest_pixels_stride_{};
    uint32_t guest_pixels_length_{};
    uint16_t max_full_frame_fps_{};
    bool rgb565_byte_swapped_{};
    bool direct_scanout_{};

    friend class Event;
    friend class Renderer;
};

// DirectSurface whose buffers live in Host memory. The Guest never maps them:
// it uploads INDEX8 resources once through RasterResources and draws each frame
// by handing the Host a RasterDrawList (Graphics 1.6). Needs no pinned Guest
// memory. Requires RendererInfo::raster_supported() to draw anything.
class HostSurface final : public DirectSurface {
   public:
    constexpr HostSurface() = default;
    HostSurface(HostSurface&& other) noexcept = default;
    HostSurface& operator=(HostSurface&& other) noexcept = default;
    ~HostSurface() = default;

    // Draw synchronously into a free Host buffer. The callback takes
    // RasterDrawList& and returns void. Automatically submits pending records;
    // already submitted batches are not rolled back on failure.
    template <typename Function>
    [[nodiscard]] Result<void> Update(uint32_t buffer_index, Function&& function) const;

   private:
    explicit HostSurface(const DirectSurfaceCreation& creation) : DirectSurface(creation) {}
    [[nodiscard]] RasterDrawList BeginUpdate(uint32_t buffer_index) const;

    friend class Renderer;
};

// DirectSurface whose buffers live in Guest linear memory and are written by
// the App, row-major RGB565 in the panel's byte order (rgb565_byte_swapped()).
// The Host holds a pointer into Guest memory while a buffer is in flight, so
// the Bundle must declare pinned_memory; otherwise creation returns kUnsupported.
class GuestSurface final : public DirectSurface {
   public:
    constexpr GuestSurface() = default;
    GuestSurface(GuestSurface&& other) noexcept;
    GuestSurface& operator=(GuestSurface&& other) noexcept;
    ~GuestSurface();

    [[nodiscard]] constexpr uint32_t pitch() const { return buffer_width_ * 2U; }
    [[nodiscard]] constexpr uint32_t buffer_bytes() const { return pitch() * buffer_height_; }
    // Pixels of buffer `index`, `pitch()` bytes per row; nullptr for an invalid
    // surface or index. Writing a buffer the Host holds is a data race with
    // the panel transfer, so check Busy() first.
    [[nodiscard]] uint16_t* Buffer(uint32_t index);
    [[nodiscard]] const uint16_t* Buffer(uint32_t index) const;
    // Destroys the surface and frees the buffers.
    void Reset();

   private:
    GuestSurface(const DirectSurfaceCreation& creation, uint8_t* storage);
    // Distance between consecutive buffers; each starts aligned for DMA.
    [[nodiscard]] uint32_t buffer_stride() const;

    uint8_t* storage_{};

    friend class Renderer;
};

// Storage order of an INDEX8 texture uploaded to the Host raster kernels.
enum class RasterLayout : uint8_t {
    // texel(u, v) = texels[u * height + v]; required by RasterDrawList::Column.
    kColumnMajor = 1,
    // texel(u, v) = texels[v * width + u]; required by RasterDrawList::SpanPair
    // and RasterDrawList::Warp.
    kRowMajor = 2,
};

// One entry of a warp map (RasterResources::UploadWarpMap): the texel and light
// level a target pixel takes. Front ends such as SphereView fill maps with
// these; Apps only need them to add their own decoration (halos, vignettes).
struct WarpEntry final {
    // The pixel is left alone (or takes the record's fill color).
    static constexpr uint32_t kSkip = 0x80000000U;
    static constexpr uint32_t kSolid = 0x40000000U;
    static constexpr uint32_t kMaxLight = 31U;
    static constexpr uint32_t kMaxCoordinate = 4095U;
    // Texel (u, v) of the record's texture, lit at `light`; u and v wrap on
    // the texture size after the record's offsets are added.
    static constexpr uint32_t Texel(uint32_t u, uint32_t v, uint32_t light) {
        return ((light & kMaxLight) << 24U) | ((v & kMaxCoordinate) << 12U) | (u & kMaxCoordinate);
    }
    // Palette entry `index` at `light`, without sampling the texture.
    static constexpr uint32_t Solid(uint8_t index, uint32_t light) {
        return kSolid | ((light & kMaxLight) << 24U) | index;
    }
};

// One corner of a RasterDrawList::Triangle / Quad. Screen position in 12.4
// fixed-point buffer pixels (sub-pixel bits keep slow edges from jittering),
// texel coordinates in 8.8 fixed point (integer part wraps on the texture
// size) and the lit palette level at this corner. Build with
// RasterVertex::At() from float pixel / texel values.
struct RasterVertex final {
    static constexpr int32_t kPositionScale = 16;   // 12.4
    static constexpr int32_t kTexelScale = 256;     // 8.8
    static constexpr float kMaxPosition = 2047.0F;  // int16 range of 12.4

    int16_t x{};
    int16_t y{};
    uint16_t u{};
    uint16_t v{};
    uint8_t light{};

    // Positions are clamped to the representable range (the Host clips the
    // polygon anyway); texel coordinates wrap on 256.
    [[nodiscard]] static RasterVertex At(float x, float y, float u, float v, uint8_t light) {
        RasterVertex vertex{};
        vertex.x = Position(x);
        vertex.y = Position(y);
        vertex.u = Texel(u);
        vertex.v = Texel(v);
        vertex.light = light;
        return vertex;
    }

   private:
    // Rounds toward negative infinity without libm (the SDK links none).
    [[nodiscard]] static int32_t FloorToInt(float value) {
        const auto truncated = static_cast<int32_t>(value);
        return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
    }
    [[nodiscard]] static int16_t Position(float value) {
        if (value > kMaxPosition) value = kMaxPosition;
        if (value < -kMaxPosition) value = -kMaxPosition;
        return static_cast<int16_t>(FloorToInt(value * static_cast<float>(kPositionScale) + 0.5F));
    }
    [[nodiscard]] static uint16_t Texel(float value) {
        return static_cast<uint16_t>(FloorToInt(value * static_cast<float>(kTexelScale)) & 0xFFFF);
    }
};

// Draw list for one HostSurface buffer (Graphics 1.6). Records are encoded
// into a fixed wire buffer and handed to the Host, which rasterizes them
// synchronously into the buffer; a full wire buffer is flushed automatically,
// so a list may hold any number of records. Only one list is open at a time,
// scoped to HostSurface::Update(). Coordinates are in buffer
// pixels. Column/SpanPair/Span must lie inside the buffer (the Host rejects
// the list otherwise); Sprite/Rect/Warp/Image/Text are clipped by the Host. Colors are
// canonical RGB565 whatever the panel's byte order. Textured records name the
// palette slot they are lit from; SetPalette() changes the default (slot 0)
// for the records that follow.
class RasterDrawList final {
   public:
    RasterDrawList(const RasterDrawList&) = delete;
    RasterDrawList& operator=(const RasterDrawList&) = delete;
    RasterDrawList(RasterDrawList&&) = delete;
    RasterDrawList& operator=(RasterDrawList&&) = delete;
    ~RasterDrawList();

    // Palette slot used by Column/SpanPair/Span/Sprite/Warp records appended after
    // this call. Lists start at slot 0.
    void SetPalette(uint8_t palette_slot) { palette_slot_ = palette_slot; }
    [[nodiscard]] constexpr uint8_t palette() const { return palette_slot_; }

    // Vertical run x, y0..y1 inclusive from column `u` of a kColumnMajor
    // texture at palette level `light_level`. `v` is 16.16 texture rows starting at
    // v_start and advancing v_step per pixel, wrapped on the texture height.
    // `transparent` skips texels with index 0.
    [[nodiscard]] bool Column(uint16_t x, int16_t y0, int16_t y1, uint8_t texture_slot, uint8_t light_level, uint16_t u,
                              int32_t v_start, int32_t v_step, bool transparent = false);
    // Row y_floor from floor_texture_slot and row y_ceiling from ceiling_texture_slot
    // (both kRowMajor) over x0..x1 inclusive, sampled at the same 16.16 (s, t)
    // walk: the integer part is the tile and the fraction selects the texel.
    [[nodiscard]] bool SpanPair(uint16_t y_floor, uint16_t y_ceiling, uint16_t x0, uint16_t x1,
                                uint8_t floor_texture_slot, uint8_t ceiling_texture_slot, uint8_t light_level,
                                int32_t s, int32_t t, int32_t ds, int32_t dt);
    // One row y over x0..x1 inclusive from a kRowMajor texture with the same
    // (s, t) walk as SpanPair. Perspective ground planes (Mode7Plane) emit one
    // per screen row, since every row has its own depth and step.
    [[nodiscard]] bool Span(uint16_t y, uint16_t x0, uint16_t x1, uint8_t texture_slot, uint8_t light_level, int32_t s,
                            int32_t t, int32_t ds, int32_t dt);
    // Texels (u0.., v0..) of size src of a kColumnMajor texture, scaled with
    // nearest-neighbour sampling onto `destination` (clipped to the buffer),
    // lit at `light_level`. `transparent` skips texel index 0. Weapons, HUD icons.
    [[nodiscard]] bool Sprite(Rect destination, uint8_t texture_slot, uint8_t light_level, uint16_t u0, uint16_t v0,
                              uint16_t source_width, uint16_t source_height, bool transparent = true);
    // Every entry of warp map `warp_slot` (RasterResources::UploadWarpMap) with
    // entry (0, 0) at `origin`, sampling kRowMajor `texture_slot` (power-of-two
    // size) at the entry's ((u + u_offset) >> u_fraction_bits, v + v_offset),
    // wrapped: with u_fraction_bits (0..4) the low bits of the map's u and of
    // u_offset are a texel fraction, so the texture can scroll in sub-texel
    // steps (texture width << u_fraction_bits must stay within 4096). Skipped
    // entries leave the pixel alone, or take `fill` when it is given. One
    // record draws a whole sphere or planar ground; per frame only the offsets
    // change.
    [[nodiscard]] bool Warp(Point origin, uint8_t warp_slot, uint8_t texture_slot, uint16_t u_offset = 0U,
                            uint16_t v_offset = 0U, uint8_t u_fraction_bits = 0U);
    [[nodiscard]] bool Warp(Point origin, uint8_t warp_slot, uint8_t texture_slot, Color fill, uint16_t u_offset = 0U,
                            uint16_t v_offset = 0U, uint8_t u_fraction_bits = 0U);
    // Draw a shared Texture directly. Source uses Texture coordinates;
    // destination uses physical buffer pixels, like the other Raster methods.
    [[nodiscard]] bool Image(const Texture& texture, Rect destination, Rect source, uint8_t opacity = 255);
    // Like Sprite but every drawn texel writes `color` (glyph atlases,
    // monochrome overlays); the palette is not consulted.
    [[nodiscard]] bool SolidSprite(Rect destination, uint8_t texture_slot, Color color, uint16_t u0, uint16_t v0,
                                   uint16_t source_width, uint16_t source_height, bool transparent = true);
    // Fills `area` (clipped) with `color`; alpha 255 writes, less blends over
    // the existing pixels. alpha 0 is rejected.
    [[nodiscard]] bool FillRect(Rect area, Color color, uint8_t alpha = 255U);
    // Draws `text` (UTF-8, 1..1024 bytes, no trailing NUL needed) with a
    // system font or a loaded Font, top-left at `origin`, clipped; glyph
    // coverage is blended over the existing pixels. Layout and metrics are
    // those of Renderer::MeasureText, so an App can centre or right-align.
    [[nodiscard]] bool Text(Point origin, const char* text, Color color, SystemFont font = SystemFont::kMedium);
    [[nodiscard]] bool Text(Point origin, const char* text, Color color, const Font& font);

    // Affine textured polygons (RendererInfo::polygon_supported()). Corners
    // may be in either winding; a Quad must be convex. u/v and light are
    // interpolated along the edges and across each scanline without
    // perspective correction, sampling a kRowMajor power-of-two texture
    // (floor(u) mod width, floor(v) mod height) at the interpolated level of
    // the current palette. `transparent` skips texel index 0. The Host clips;
    // zero-area polygons draw nothing.
    [[nodiscard]] bool Triangle(const RasterVertex (&corners)[3], uint8_t texture_slot, bool transparent = false);
    [[nodiscard]] bool Quad(const RasterVertex (&corners)[4], uint8_t texture_slot, bool transparent = false);
    // Untextured variants: every pixel takes palette entry `color_index` at
    // the interpolated light level (the corners' u/v are ignored).
    [[nodiscard]] bool FlatTriangle(const RasterVertex (&corners)[3], uint8_t color_index);
    [[nodiscard]] bool FlatQuad(const RasterVertex (&corners)[4], uint8_t color_index);

   private:
    explicit RasterDrawList(int32_t status) : status_(status) {}
    [[nodiscard]] Result<void> Finish();
    [[nodiscard]] constexpr bool open() const { return open_; }
    RasterDrawList(uint32_t surface_handle, uint32_t buffer_index);
    [[nodiscard]] bool Append(const void* record, uint32_t size);
    [[nodiscard]] bool AppendText(Point origin, const char* text, Color color, uint32_t font_handle);
    void Flush();
    void Close();

    uint32_t surface_handle_{};
    uint32_t target_buffer_{};
    bool open_{};
    uint8_t palette_slot_{};
    uint16_t record_count_{};
    uint32_t wire_size_{};
    int32_t status_{};

    friend class HostSurface;
};

template <typename Function>
Result<void> HostSurface::Update(uint32_t buffer_index, Function&& function) const {
    static_assert(std::is_invocable_r_v<void, Function, RasterDrawList&>,
                  "Surface Update callback must accept RasterDrawList&");
    static_assert(std::is_same_v<std::invoke_result_t<Function, RasterDrawList&>, void>,
                  "Surface Update callback must return void");
    auto list = BeginUpdate(buffer_index);
    if (!list.open()) return list.Finish();
    static_cast<Function&&>(function)(list);
    return list.Finish();
}

// Upload entry for the Host raster kernels' resources (Graphics 1.6): INDEX8
// textures, lit palettes and warp maps that HostSurface draw lists sample.
// The Guest uploads them once, then per frame casts its geometry and emits
// Column/SpanPair/Sprite/Rect/Warp records instead of writing pixels. The
// per-pixel loops run natively on the Host, on the Guest task.
//
// Resources belong to the App session, not to a surface: they survive
// destroying and recreating a HostSurface (for example to show a Scene in
// between) and are released when the App exits. Every slot kind is an
// independent uint8 ID space.
class RasterResources final {
   public:
    // A default-constructed value has no kernels: valid() is false and every
    // operation fails with kUnsupported. Obtain a usable one from
    // Renderer::CreateRasterResources().
    constexpr RasterResources() noexcept = default;
    constexpr RasterResources(const RasterResources&) noexcept = default;
    constexpr RasterResources& operator=(const RasterResources&) noexcept = default;

    [[nodiscard]] constexpr bool valid() const { return enabled_; }

    // Entries per light level of a lit palette; fixed by the INDEX8 texel format.
    static constexpr uint32_t kPaletteEntries = 256U;

    // Every upload takes exactly the buffer its dimensions describe; a span of
    // any other size fails with kInvalidArgument before reaching the Host.
    // Slots are reusable uint8 IDs. OOM leaves an occupied slot unchanged.

    // `texels` holds width * height INDEX8 values in `layout`; width and height
    // must be in 1..65535.
    [[nodiscard]] Result<void> UploadTexture(uint8_t texture_slot, uint32_t width, uint32_t height, RasterLayout layout,
                                             std::span<const uint8_t> texels) const;
    // `entries` holds light_levels x kPaletteEntries canonical RGB565 pixels,
    // entry [light_level][index] being the pixel written for texel `index`.
    // The Host converts to the panel byte order. A world palette with many
    // light levels can sit in one slot and a flat sprite palette in another.
    [[nodiscard]] Result<void> UploadLitPalette(uint8_t palette_slot, uint32_t light_levels,
                                                std::span<const uint16_t> entries) const;
    // `entries` holds width * height WarpEntry values, one per target pixel of
    // a RasterDrawList::Warp record. Replaces whatever the slot held.
    [[nodiscard]] Result<void> UploadWarpMap(uint8_t warp_slot, uint32_t width, uint32_t height,
                                             std::span<const uint32_t> entries) const;
    // Writes rows first_row..first_row+row_count-1 (`entries` holds
    // row_count * width values) of a width x height map. A slot already
    // holding a map of that size is updated in place; otherwise a new map is
    // allocated whose other rows are WarpEntry::kSkip, so a large map can be
    // streamed in over several frames without a full-size Guest buffer.
    [[nodiscard]] Result<void> UpdateWarpRows(uint8_t warp_slot, uint32_t width, uint32_t height, uint32_t first_row,
                                              uint32_t row_count, std::span<const uint32_t> entries) const;

   private:
    explicit constexpr RasterResources(bool enabled) noexcept : enabled_(enabled) {}

    bool enabled_{};

    friend class Renderer;
};

class Renderer final {
   public:
    constexpr Renderer(const Renderer&) noexcept = default;
    constexpr Renderer& operator=(const Renderer&) noexcept = default;

    [[nodiscard]] RendererInfo info() const;
    [[nodiscard]] Result<Scene> CreateScene(Color background = Color::Black()) const;
    // Publishes pending Scene changes. Failure preserves both the displayed
    // frame and pending Guest state for retry.
    [[nodiscard]] Result<void> Present(const Scene& scene) const;
    [[nodiscard]] Result<Scene> CreateScene(const SceneDescriptor& descriptor) const;
    // One DirectSurface (of either kind) per App at a time. `upscale` shrinks
    // the buffers to physical size / upscale in both axes (must divide both
    // exactly) and the Host enlarges each presented frame (PPA where
    // available, nearest neighbour otherwise). Host raster kernels then write
    // 1/upscale^2 of the pixels, which is what a 720x720 panel needs to stay
    // above 30 fps.
    [[nodiscard]] Result<HostSurface> CreateHostSurface(uint32_t buffer_count = 2U, uint32_t upscale = 1U) const;
    // Guest-written buffers; kUnsupported unless the Bundle declares pinned_memory.
    [[nodiscard]] Result<GuestSurface> CreateGuestSurface(uint32_t buffer_count = 2U, uint32_t upscale = 1U) const;
    // Host raster kernels; kUnsupported when RendererInfo::raster_supported()
    // is false (older Host or the pool is configured to 0).
    [[nodiscard]] Result<RasterResources> CreateRasterResources() const;
    [[nodiscard]] Result<TextMetrics> MeasureText(const char* text, SystemFont font = SystemFont::kMedium) const;
    [[nodiscard]] Result<TextMetrics> MeasureText(const char* text, const Font& font) const;

   private:
    struct CapabilityToken {};
    explicit constexpr Renderer(CapabilityToken) noexcept {}
    friend class Application;
    friend class Container;
    friend class ui::ImageButton;
    friend class ui::Label;
    friend class ui::TextButton;
};

inline const SurfaceReleasedEvent* Event::ReleasedFrom(const DirectSurface& source) const {
    const SurfaceReleasedEvent* candidate = surface_released();
    return candidate != nullptr && source.valid() && candidate->source_ == source.handle_ ? candidate : nullptr;
}

}  // namespace micropixel

#include "sdk/scene.hpp"

#endif
