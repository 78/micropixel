#ifndef MICROPIXEL_SDK_RESOURCES_HPP
#define MICROPIXEL_SDK_RESOURCES_HPP

#include <stdint.h>

#include <span>

#include "sdk/graphics.hpp"
#include "sdk/result.hpp"

namespace micropixel {

class Application;
class Scene;
class Renderer;
class Resources;
class Font;

class AssetId final {
   public:
    explicit constexpr AssetId(uint32_t value) : value_(value) {}
    [[nodiscard]] constexpr uint32_t value() const { return value_; }

   private:
    uint32_t value_{};
};

class Texture final {
   public:
    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;
    ~Texture();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr uint32_t width() const { return width_; }
    [[nodiscard]] constexpr uint32_t height() const { return height_; }
    // Dynamic textures publish new pixels on the next Present. Existing scene
    // references follow updates automatically; failed updates retain old pixels.
    [[nodiscard]] Result<void> Update(Rect dirty, std::span<const uint8_t> pixels, uint32_t pitch);
    void Reset();

   private:
    constexpr Texture(uint32_t handle, uint32_t width, uint32_t height, uint32_t physical_width,
                      uint32_t physical_height, bool adaptive)
        : handle_(handle),
          width_(width),
          height_(height),
          physical_width_(physical_width),
          physical_height_(physical_height),
          adaptive_(adaptive) {}

    uint32_t handle_{};
    uint32_t width_{};
    uint32_t height_{};
    uint32_t physical_width_{};
    uint32_t physical_height_{};
    bool adaptive_{};

    friend class Container;
    friend class SpriteNode;
    friend class SpriteBatch;
    friend class RasterDrawList;
    friend class Renderer;
    friend class Resources;
    friend struct Material3D;
};

class Font final {
   public:
    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&& other) noexcept;
    Font& operator=(Font&& other) noexcept;
    ~Font();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr uint16_t size() const { return size_; }
    [[nodiscard]] constexpr uint16_t line_height() const { return line_height_; }
    [[nodiscard]] constexpr int16_t ascent() const { return ascent_; }
    [[nodiscard]] constexpr int16_t descent() const { return descent_; }
    void Reset();

   private:
    constexpr Font(uint32_t handle, uint16_t size, uint16_t line_height, int16_t ascent, int16_t descent)
        : handle_(handle), size_(size), line_height_(line_height), ascent_(ascent), descent_(descent) {}

    uint32_t handle_{};
    uint16_t size_{};
    uint16_t line_height_{};
    int16_t ascent_{};
    int16_t descent_{};

    friend class Container;
    friend class RasterDrawList;
    friend class Renderer;
    friend class Resources;
};

// Native preserves authored pixels for shared Scene/Raster sampling. Display
// scales to the panel (the logical-canvas adaptation 2D UI uses). Surface
// scales to the App's live Direct Surface buffers (display scale divided by
// the surface's integer upscale) so unscaled Image records copy rows verbatim;
// it fails with InvalidArgument when no surface exists.
enum class TextureScale : uint8_t { kNative, kDisplay, kSurface };

class Resources final {
   public:
    [[nodiscard]] Result<Texture> LoadTexture(AssetId asset, TextureScale scale = TextureScale::kNative) const;
    [[nodiscard]] Result<Font> LoadFont(AssetId asset) const;
    [[nodiscard]] Result<Texture> CreateDynamicTexture(Size size, PixelFormat format,
                                                       std::span<const uint8_t> pixels = {}, uint32_t pitch = 0) const;

   private:
    struct CapabilityToken {};
    explicit constexpr Resources(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
