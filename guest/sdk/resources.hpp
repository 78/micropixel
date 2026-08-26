#ifndef MICROPIXEL_SDK_RESOURCES_HPP
#define MICROPIXEL_SDK_RESOURCES_HPP

#include <stdint.h>

#include "sdk/graphics.hpp"
#include "sdk/result.hpp"

namespace micropixel {

class Application;
class Frame;
class Renderer;
class Resources;
class StreamingTexture;
class Font;

class AssetId final {
   public:
    explicit constexpr AssetId(uint32_t value) : value_(value) {}
    [[nodiscard]] constexpr uint32_t value() const { return value_; }

   private:
    uint32_t value_{};
};

class ResourceRef final {
   public:
    [[nodiscard]] static constexpr ResourceRef Package(AssetId asset) { return ResourceRef{asset}; }
    [[nodiscard]] constexpr AssetId asset() const { return asset_; }

   private:
    explicit constexpr ResourceRef(AssetId asset) : asset_(asset) {}
    AssetId asset_{0U};
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
    void Reset();

   private:
    constexpr Texture(uint32_t handle, uint32_t width, uint32_t height)
        : handle_(handle), width_(width), height_(height) {}

    uint32_t handle_{};
    uint32_t width_{};
    uint32_t height_{};

    friend class Frame;
    friend class Renderer;
    friend class Resources;
    friend class StreamingTexture;
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
    constexpr Font(uint16_t handle, uint16_t size, uint16_t line_height, int16_t ascent, int16_t descent)
        : handle_(handle), size_(size), line_height_(line_height), ascent_(ascent), descent_(descent) {}

    uint16_t handle_{};
    uint16_t size_{};
    uint16_t line_height_{};
    int16_t ascent_{};
    int16_t descent_{};

    friend class Frame;
    friend class Renderer;
    friend class Resources;
};

class StreamingTexture final {
   public:
    StreamingTexture() = default;
    StreamingTexture(const StreamingTexture&) = delete;
    StreamingTexture& operator=(const StreamingTexture&) = delete;
    StreamingTexture(StreamingTexture&&) noexcept = default;
    StreamingTexture& operator=(StreamingTexture&&) noexcept = default;

    [[nodiscard]] constexpr bool valid() const { return texture_.valid(); }
    [[nodiscard]] constexpr uint32_t width() const { return texture_.width(); }
    [[nodiscard]] constexpr uint32_t height() const { return texture_.height(); }
    [[nodiscard]] constexpr PixelFormat pixel_format() const { return pixel_format_; }
    [[nodiscard]] Result<void> Update(Rect dirty, const uint8_t* pixels, uint32_t byte_length, uint32_t pitch);
    void Reset() { texture_.Reset(); }

   private:
    StreamingTexture(Texture texture, PixelFormat pixel_format)
        : texture_(static_cast<Texture&&>(texture)), pixel_format_(pixel_format) {}

    Texture texture_{};
    PixelFormat pixel_format_{PixelFormat::kBgr888};

    friend class Frame;
    friend class Renderer;
};

class TextureUpdateBatch final {
   public:
    TextureUpdateBatch(const TextureUpdateBatch&) = delete;
    TextureUpdateBatch& operator=(const TextureUpdateBatch&) = delete;
    TextureUpdateBatch(TextureUpdateBatch&& other) noexcept;
    TextureUpdateBatch& operator=(TextureUpdateBatch&&) = delete;
    ~TextureUpdateBatch();

    [[nodiscard]] Result<void> Finish();

   private:
    struct CapabilityToken {};
    explicit constexpr TextureUpdateBatch(CapabilityToken) : active_(true) {}
    bool active_{};

    friend class Renderer;
};

class Resources final {
   public:
    [[nodiscard]] Result<Texture> LoadTexture(ResourceRef resource) const;
    [[nodiscard]] Result<Font> LoadFont(ResourceRef resource) const;

   private:
    struct CapabilityToken {};
    explicit constexpr Resources(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
