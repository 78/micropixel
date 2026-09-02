#ifndef MICROPIXEL_SDK_GRAPHICS_HPP
#define MICROPIXEL_SDK_GRAPHICS_HPP

#include <stdint.h>

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

   private:
    constexpr RendererInfo(uint32_t width, uint32_t height, uint32_t physical_width, uint32_t physical_height,
                           DisplayInsets safe_area_insets, uint16_t max_scene_nodes, uint16_t max_batch_instances,
                           uint16_t max_containers, uint16_t max_sprite_batches, uint32_t max_scene_bytes)
        : width_(width),
          height_(height),
          physical_width_(physical_width),
          physical_height_(physical_height),
          safe_area_insets_(safe_area_insets),
          max_scene_bytes_(max_scene_bytes),
          max_scene_nodes_(max_scene_nodes),
          max_batch_instances_(max_batch_instances),
          max_containers_(max_containers),
          max_sprite_batches_(max_sprite_batches) {}

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

}  // namespace micropixel

#include "sdk/scene.hpp"

#endif
