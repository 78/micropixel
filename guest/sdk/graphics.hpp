#ifndef MICROPIXEL_SDK_GRAPHICS_HPP
#define MICROPIXEL_SDK_GRAPHICS_HPP

#include <stdint.h>

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
class SurfaceNode;
class LabelNode;
class SpriteBatch;
struct SceneDescriptor;
class Texture;
class StreamingTexture;
class TextureUpdateBatch;
class DirectSurface;
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
    [[nodiscard]] constexpr uint16_t max_scene_nodes() const { return max_scene_nodes_; }
    [[nodiscard]] constexpr uint16_t max_batch_instances() const { return max_batch_instances_; }
    [[nodiscard]] constexpr uint16_t max_containers() const { return max_containers_; }
    [[nodiscard]] constexpr uint16_t max_sprite_batches() const { return max_sprite_batches_; }
    [[nodiscard]] constexpr uint32_t max_scene_bytes() const { return max_scene_bytes_; }
    // True when a DirectSurface frame reaches the panel without an App Surface
    // copy. False Hosts still accept DirectSurface but composite every frame.
    [[nodiscard]] constexpr bool direct_scanout() const { return direct_scanout_; }
    // The panel consumes RGB565 with both bytes of every pixel swapped. A Guest
    // that writes DirectSurface buffers itself must write that order.
    [[nodiscard]] constexpr bool rgb565_byte_swapped() const { return rgb565_byte_swapped_; }
    // Panel transfer bound for one full frame; 0 when the Host does not know.
    [[nodiscard]] constexpr uint16_t max_full_frame_fps() const { return max_full_frame_fps_; }
    // Host raster kernels (SurfaceRaster) are available; raster_pool_bytes() is
    // the byte quota for uploaded textures plus palette.
    [[nodiscard]] constexpr bool raster_supported() const { return raster_pool_bytes_ != 0U; }
    [[nodiscard]] constexpr uint32_t raster_pool_bytes() const { return raster_pool_bytes_; }

   private:
    constexpr RendererInfo(uint32_t width, uint32_t height, uint32_t physical_width, uint32_t physical_height,
                           DisplayInsets safe_area_insets, uint16_t max_scene_nodes, uint16_t max_batch_instances,
                           uint16_t max_containers, uint16_t max_sprite_batches, uint32_t max_scene_bytes,
                           bool direct_scanout, bool rgb565_byte_swapped, uint16_t max_full_frame_fps,
                           uint32_t raster_pool_bytes)
        : width_(width),
          height_(height),
          physical_width_(physical_width),
          physical_height_(physical_height),
          safe_area_insets_(safe_area_insets),
          max_scene_bytes_(max_scene_bytes),
          max_scene_nodes_(max_scene_nodes),
          max_batch_instances_(max_batch_instances),
          max_containers_(max_containers),
          max_sprite_batches_(max_sprite_batches),
          max_full_frame_fps_(max_full_frame_fps),
          direct_scanout_(direct_scanout),
          rgb565_byte_swapped_(rgb565_byte_swapped),
          raster_pool_bytes_(raster_pool_bytes) {}

    uint32_t width_{};
    uint32_t height_{};
    uint32_t physical_width_{};
    uint32_t physical_height_{};
    DisplayInsets safe_area_insets_{};
    uint32_t max_scene_bytes_{};
    uint16_t max_scene_nodes_{};
    uint16_t max_batch_instances_{};
    uint16_t max_containers_{};
    uint16_t max_sprite_batches_{};
    uint16_t max_full_frame_fps_{};
    bool direct_scanout_{};
    bool rgb565_byte_swapped_{};
    uint32_t raster_pool_bytes_{};

    friend class Renderer;
};

// Who owns the pixels of a DirectSurface.
enum class DirectSurfaceBuffers : uint8_t {
    // The Host allocates the buffers in its own memory and the Guest never
    // maps them: it draws through SurfaceRaster draw lists and presents by
    // index. Needs no pinned Guest memory. Buffer() returns nullptr.
    kHost = 0,
    // The SDK allocates the buffers in Guest memory and the App writes pixels
    // itself, in the panel's byte order (rgb565_byte_swapped()). The Bundle
    // must declare pinned_memory.
    kGuest = 1,
};

// Full-screen RGB565 frames scanned out by the Host (Graphics 1.5). The App
// fills one buffer (itself or through SurfaceRaster), Presents it and moves on
// to a free one. A presented buffer belongs to the Host until Application
// delivers the matching EventType::kSurfaceReleased, which also updates
// AcquireFree()/Busy().
//
// A DirectSurface replaces the Scene for as long as it exists: Scene submits
// are rejected while one is created. The Host draws system UI above the frame
// and may composite instead of scanning out while that UI is visible.
class DirectSurface final {
   public:
    DirectSurface() = default;
    DirectSurface(const DirectSurface&) = delete;
    DirectSurface& operator=(const DirectSurface&) = delete;
    DirectSurface(DirectSurface&& other) noexcept;
    DirectSurface& operator=(DirectSurface&& other) noexcept;
    ~DirectSurface();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    // Panel size in physical pixels; every presented frame covers it.
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    // Size of one Guest buffer. Smaller than the panel only when the surface
    // was created with an integer upscale; the Host then enlarges the frame.
    [[nodiscard]] constexpr uint32_t buffer_width() const { return buffer_width_; }
    [[nodiscard]] constexpr uint32_t buffer_height() const { return buffer_height_; }
    [[nodiscard]] constexpr uint32_t pitch() const { return buffer_width_ * 2U; }
    [[nodiscard]] constexpr uint32_t buffer_bytes() const { return pitch() * buffer_height_; }
    [[nodiscard]] constexpr uint32_t buffer_count() const { return buffer_count_; }
    [[nodiscard]] constexpr DirectSurfaceBuffers buffers() const {
        return storage_ == nullptr ? DirectSurfaceBuffers::kHost : DirectSurfaceBuffers::kGuest;
    }
    [[nodiscard]] constexpr bool host_buffers() const { return buffers() == DirectSurfaceBuffers::kHost; }
    // Byte order of every presented pixel; Host buffers are kept in it and a
    // kGuest App writes it (swap the two bytes of each RGB565 value when true).
    [[nodiscard]] constexpr bool rgb565_byte_swapped() const { return rgb565_byte_swapped_; }
    [[nodiscard]] constexpr bool direct_scanout() const { return direct_scanout_; }
    [[nodiscard]] constexpr uint16_t max_full_frame_fps() const { return max_full_frame_fps_; }

    // kGuest only: row-major RGB565, `pitch()` bytes per row; nullptr for a
    // Host-buffer surface, an invalid surface or index. Writing a buffer the
    // Host holds is a data race with the panel transfer, so check Busy() first.
    [[nodiscard]] uint16_t* Buffer(uint32_t index);
    [[nodiscard]] const uint16_t* Buffer(uint32_t index) const;
    [[nodiscard]] bool Busy(uint32_t index) const;
    // Lowest-numbered buffer the Host does not hold. False when every buffer is
    // in flight; wait for kSurfaceReleased before rendering again.
    [[nodiscard]] bool AcquireFree(uint32_t& index_out) const;
    // Hands `index` to the Host. Fails with kInvalidState when the buffer is
    // still held by the Host.
    [[nodiscard]] Result<void> Present(uint32_t index);
    // Returns every buffer and destroys the surface; Scene submits work again.
    void Reset();

   private:
    DirectSurface(uint32_t handle, uint32_t width, uint32_t height, uint32_t buffer_width, uint32_t buffer_height,
                  uint32_t buffer_count, uint8_t* storage, uint32_t native_flags, uint16_t max_full_frame_fps);

    uint32_t handle_{};
    uint32_t width_{};
    uint32_t height_{};
    uint32_t buffer_width_{};
    uint32_t buffer_height_{};
    uint32_t buffer_count_{};
    uint8_t* storage_{};
    uint16_t max_full_frame_fps_{};
    bool rgb565_byte_swapped_{};
    bool direct_scanout_{};

    friend class Renderer;
    friend class Event;
};

// Storage order of an INDEX8 texture uploaded to the Host raster kernels.
enum class RasterLayout : uint8_t {
    // texel(u, v) = texels[u * height + v]; required by RasterDrawList::Column.
    kColumnMajor = 1,
    // texel(u, v) = texels[v * width + u]; required by RasterDrawList::SpanPair.
    kRowMajor = 2,
};

// Draw list for one Host-buffer DirectSurface buffer (Graphics 1.6). Records
// are encoded into a fixed wire buffer and handed to the Host, which
// rasterizes them synchronously into the buffer; a full wire buffer is flushed
// automatically, so a list may hold any number of records. Only one list is
// open at a time; Begin() a new one after Finish(). Coordinates are in buffer
// pixels. Column/SpanPair must lie inside the buffer (the Host rejects the
// list otherwise); Sprite/Rect are clipped by the Host. Colors are canonical
// RGB565 whatever the panel's byte order.
class RasterDrawList final {
   public:
    RasterDrawList() = default;
    RasterDrawList(const RasterDrawList&) = delete;
    RasterDrawList& operator=(const RasterDrawList&) = delete;
    RasterDrawList(RasterDrawList&& other) noexcept;
    RasterDrawList& operator=(RasterDrawList&& other) noexcept;
    // An open list dropped without Finish() discards its pending records.
    ~RasterDrawList();

    // Vertical run x, y0..y1 inclusive from column `u` of a kColumnMajor
    // texture at palette light `light`. `v` is 16.16 texture rows starting at
    // v_start and advancing v_step per pixel, wrapped on the texture height.
    // `transparent` skips texels with index 0.
    [[nodiscard]] bool Column(uint16_t x, int16_t y0, int16_t y1, uint8_t texture, uint8_t light, uint16_t u,
                              int32_t v_start, int32_t v_step, bool transparent = false);
    // Row y_floor from floor_texture and row y_ceiling from ceiling_texture
    // (both kRowMajor) over x0..x1 inclusive, sampled at the same 16.16 (s, t)
    // walk: the integer part is the tile and the fraction selects the texel.
    [[nodiscard]] bool SpanPair(uint16_t y_floor, uint16_t y_ceiling, uint16_t x0, uint16_t x1, uint8_t floor_texture,
                                uint8_t ceiling_texture, uint8_t light, int32_t s, int32_t t, int32_t ds, int32_t dt);
    // Texels (u0.., v0..) of size src of a kColumnMajor texture, scaled with
    // nearest-neighbour sampling onto `destination` (clipped to the buffer),
    // lit at `light`. `transparent` skips texel index 0. Weapons, HUD icons.
    [[nodiscard]] bool Sprite(Rect destination, uint8_t texture, uint8_t light, uint16_t u0, uint16_t v0,
                              uint16_t src_width, uint16_t src_height, bool transparent = true);
    // Like Sprite but every drawn texel writes `color` (glyph atlases,
    // monochrome overlays); the palette is not consulted.
    [[nodiscard]] bool SolidSprite(Rect destination, uint8_t texture, Color color, uint16_t u0, uint16_t v0,
                                   uint16_t src_width, uint16_t src_height, bool transparent = true);
    // Fills `area` (clipped) with `color`; alpha 255 writes, less blends over
    // the existing pixels. alpha 0 is rejected.
    [[nodiscard]] bool FillRect(Rect area, Color color, uint8_t alpha = 255U);
    // Submits the pending records. Returns the first error of any flush; the
    // list is closed either way.
    [[nodiscard]] Result<void> Finish();
    [[nodiscard]] constexpr bool open() const { return open_; }

   private:
    RasterDrawList(uint32_t target_buffer, uint16_t width, uint16_t height, uint16_t pitch);
    [[nodiscard]] bool Append(const void* record, uint32_t size);
    void Flush();
    void Close();

    uint32_t target_buffer_{};
    bool open_{};
    uint16_t width_{};
    uint16_t height_{};
    uint16_t pitch_{};
    uint16_t record_count_{};
    uint32_t wire_size_{};
    int32_t status_{};

    friend class SurfaceRaster;
};

// Host-side raster kernels for 2.5D software renderers (Graphics 1.6): the
// Guest uploads INDEX8 textures and a lit palette once, then per frame casts
// its geometry and emits Column/SpanPair/Sprite/Rect records instead of
// writing pixels. The per-pixel loops run natively on the Host, on the Guest
// task, into a Host-buffer DirectSurface. Resources live until the App exits.
class SurfaceRaster final {
   public:
    // A default-constructed value has no kernels: valid() is false and every
    // operation fails with kUnsupported. Obtain a usable one from
    // Renderer::CreateSurfaceRaster().
    constexpr SurfaceRaster() noexcept = default;
    constexpr SurfaceRaster(const SurfaceRaster&) noexcept = default;
    constexpr SurfaceRaster& operator=(const SurfaceRaster&) noexcept = default;

    [[nodiscard]] constexpr bool valid() const { return pool_bytes_ != 0U; }
    // Host quota shared by every texture and the palette, in bytes.
    [[nodiscard]] constexpr uint32_t pool_bytes() const { return pool_bytes_; }
    [[nodiscard]] constexpr uint32_t max_textures() const { return max_textures_; }
    [[nodiscard]] constexpr uint32_t max_light_levels() const { return max_light_levels_; }

    // `texels` holds width * height INDEX8 values in `layout`; width and height
    // are powers of two from 8 to 128. Uploading to a used slot replaces it.
    [[nodiscard]] Result<void> UploadTexture(uint8_t slot, uint32_t width, uint32_t height, RasterLayout layout,
                                             const uint8_t* texels) const;
    // `entries` holds light_levels x 256 canonical RGB565 pixels, entry
    // [light][index] being the pixel written for texel `index`. The Host
    // converts to the panel byte order.
    [[nodiscard]] Result<void> UploadLitPalette(uint32_t light_levels, const uint16_t* entries) const;
    // Opens a draw list on buffer `buffer_index` of a Host-buffer surface; the
    // buffer must not be held by the Host (Busy()). Records go to the Host on
    // Finish() or when the wire buffer fills. Returns a closed list for a
    // kGuest surface.
    [[nodiscard]] RasterDrawList Begin(DirectSurface& surface, uint32_t buffer_index) const;

   private:
    constexpr SurfaceRaster(uint32_t pool_bytes, uint32_t max_textures, uint32_t max_light_levels) noexcept
        : pool_bytes_(pool_bytes), max_textures_(max_textures), max_light_levels_(max_light_levels) {}

    uint32_t pool_bytes_{};
    uint32_t max_textures_{};
    uint32_t max_light_levels_{};

    friend class Renderer;
};

class Renderer final {
   public:
    constexpr Renderer(const Renderer&) noexcept = default;
    constexpr Renderer& operator=(const Renderer&) noexcept = default;

    [[nodiscard]] RendererInfo info() const;
    [[nodiscard]] Scene CreateScene(Color background = Color::Black()) const;
    [[nodiscard]] Scene CreateScene(const SceneDescriptor& descriptor) const;
    [[nodiscard]] Result<StreamingTexture> CreateStreamingTexture(Size size, PixelFormat pixel_format) const;
    [[nodiscard]] TextureUpdateBatch BeginTextureUpdateBatch() const;
    // One DirectSurface per App at a time. `upscale` shrinks the buffers to
    // physical size / upscale in both axes (must divide both exactly) and the
    // Host enlarges each presented frame (PPA where available, nearest
    // neighbour otherwise). Host raster kernels then write 1/upscale^2 of
    // the pixels, which is what a 720x720 panel needs to stay above 30 fps.
    [[nodiscard]] Result<DirectSurface> CreateDirectSurface(uint32_t buffer_count = 2U,
                                                            DirectSurfaceBuffers buffers = DirectSurfaceBuffers::kHost,
                                                            uint32_t upscale = 1U) const;
    // Host raster kernels; kUnsupported when RendererInfo::raster_supported()
    // is false (older Host or the pool is configured to 0).
    [[nodiscard]] Result<SurfaceRaster> CreateSurfaceRaster() const;
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
