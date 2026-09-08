#ifndef MICROPIXEL_SDK_SCENE_HPP
#define MICROPIXEL_SDK_SCENE_HPP

#include <stdint.h>

#include <memory>

namespace micropixel {

class SceneState;
class Renderer;
class Container;
class ContainerNode;
class ShapeNode;
class RoundedRectNode;
class SpriteNode;
class LabelNode;
class SpriteBatch;

namespace ui {
class FlexContainer;
struct FlexContainerProperties;
class GridContainer;
struct GridContainerProperties;
class ImageButton;
struct ImageButtonProperties;
class TextButton;
struct TextButtonProperties;
}  // namespace ui

struct SceneDescriptor final {
    uint32_t logical_width{};
    uint32_t logical_height{};
    Color background{Color::Black()};
};

struct ContainerProperties final {
    // Empty means no additional clip; the parent clip is inherited.
    Rect clip{};
    Point translation{};
    int16_t z_order{};
    uint8_t opacity{255U};
    bool visible{true};
    // Optional scrolling cache. A hint never changes compositing: the Host
    // falls back to replay when unrelated content overlaps the cached extent.
    // Mixed-frame overlays always preserve uncovered pixels.
    bool cache_content{};
};

struct RoundedRectStyle final {
    Color fill{Color::Black()};
    Color stroke{Color::Black()};
    uint32_t radius{};
    uint32_t stroke_width{};
    uint8_t opacity{255U};
};

struct SpriteInstance final {
    Rect destination{};
    Rect source{};
    Color color{Color::White()};
    uint8_t opacity{255U};
    bool visible{true};
};

class NodeHandle {
   public:
    constexpr NodeHandle() = default;
    [[nodiscard]] bool valid() const;
    // Repeated destruction of a stale handle is a
    // no-op.
    void Destroy();
    void SetVisible(bool visible);

   protected:
    constexpr NodeHandle(SceneState* state, uint16_t id, uint32_t generation)
        : state_(state), id_(id), generation_(generation) {}
    SceneState* state_{};
    uint16_t id_{};
    uint32_t generation_{};

    friend class Container;
    friend class SceneState;
};

// Common child-creation surface for the Scene root and retained containers.
// The receiver is always the parent, so child coordinates are local to it.
class Container {
   public:
    constexpr Container() = default;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] Point ToScene(Point local) const;
    [[nodiscard]] Point ToLocal(Point scene) const;

    [[nodiscard]] Result<ContainerNode> CreateContainer(const ContainerProperties& properties = {});
    [[nodiscard]] Result<ShapeNode> CreateShape(Rect rect, Color color, uint8_t opacity = 255U);
    [[nodiscard]] Result<RoundedRectNode> CreateRoundedRect(Rect rect, const RoundedRectStyle& style);
    [[nodiscard]] Result<SpriteNode> CreateSprite(const Texture& texture, Rect destination, Rect source,
                                                  uint8_t opacity = 255U);
    [[nodiscard]] Result<SpriteBatch> CreateSpriteBatch(const Texture& texture, uint16_t capacity,
                                                        uint8_t opacity = 255U);
    // A textureless batch is a batch of colored quads and maps directly to
    // accelerated fills. It is the preferred representation for grid games.
    [[nodiscard]] Result<SpriteBatch> CreateSpriteBatch(uint16_t capacity, uint8_t opacity = 255U);
    [[nodiscard]] Result<LabelNode> CreateLabel(Point position, const char* text, Color color,
                                                SystemFont font = SystemFont::kMedium, bool centered = false);
    [[nodiscard]] ui::ImageButton CreateImageButton(const Texture& texture,
                                                    const ui::ImageButtonProperties& properties);
    [[nodiscard]] ui::FlexContainer CreateFlexContainer(const ui::FlexContainerProperties& properties);
    [[nodiscard]] ui::GridContainer CreateGridContainer(const ui::GridContainerProperties& properties);
    [[nodiscard]] ui::TextButton CreateTextButton(const ui::TextButtonProperties& properties);

   protected:
    constexpr Container(SceneState* state, uint16_t id, uint32_t generation)
        : state_(state), id_(id), generation_(generation) {}
    [[nodiscard]] Result<SpriteBatch> CreateSpriteBatchInternal(uint32_t texture_handle, uint16_t capacity,
                                                                uint8_t opacity);
    [[nodiscard]] Point SceneTranslation() const;
    SceneState* state_{};
    uint16_t id_{};
    uint32_t generation_{};

    friend class SceneState;
};

// Non-drawing retained node. Containers form a tree rooted at Scene, provide a
// local coordinate space to descendants, and own their complete subtree.
class ContainerNode final : public Container {
   public:
    constexpr ContainerNode() = default;
    [[nodiscard]] Result<void> Destroy();
    void SetClip(Rect clip);
    void SetTranslation(Point translation);
    void SetOpacity(uint8_t opacity);
    void SetVisible(bool visible);
    void SetZOrder(int16_t z_order);
    // See ContainerProperties::cache_content.
    void SetCacheContent(bool cache_content);

   private:
    constexpr ContainerNode(SceneState* state, uint16_t id, uint32_t generation) : Container(state, id, generation) {}

    friend class Container;
    friend class SceneState;
    friend class NodeHandle;
};

class ShapeNode final : public NodeHandle {
   public:
    constexpr ShapeNode() = default;
    void SetRect(Rect rect);
    void SetColor(Color color);
    void SetOpacity(uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class RoundedRectNode final : public NodeHandle {
   public:
    constexpr RoundedRectNode() = default;
    void SetRect(Rect rect);
    void SetFillColor(Color color);
    void SetStrokeColor(Color color);
    void SetRadius(uint32_t radius);
    void SetStrokeWidth(uint32_t stroke_width);
    void SetOpacity(uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class SpriteNode final : public NodeHandle {
   public:
    constexpr SpriteNode() = default;
    void SetDestination(Rect destination);
    void SetSource(Rect source);
    void SetTexture(const Texture& texture);
    void SetOpacity(uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class LabelNode final : public NodeHandle {
   public:
    constexpr LabelNode() = default;
    void SetPosition(Point position);
    void SetText(const char* text);
    void SetColor(Color color);
    void SetFont(SystemFont font);
    void SetCentered(bool centered);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class SpriteBatch final : public NodeHandle {
   public:
    constexpr SpriteBatch() = default;
    [[nodiscard]] constexpr uint16_t capacity() const { return capacity_; }
    void SetTexture(const Texture& texture);
    void SetOpacity(uint8_t opacity);
    void SetInstance(uint16_t instance_id, const SpriteInstance& instance);
    void SetInstanceVisible(uint16_t instance_id, bool visible);

   private:
    constexpr SpriteBatch(SceneState* state, uint16_t id, uint32_t generation, uint16_t capacity)
        : NodeHandle(state, id, generation), capacity_(capacity) {}
    uint16_t capacity_{};

    friend class Container;
};

class Scene final : public Container {
   public:
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&&) = delete;
    ~Scene();

    void SetBackground(Color color);
    [[nodiscard]] uint16_t node_count() const;

   private:
    struct CapabilityToken {};
    explicit Scene(CapabilityToken, const SceneDescriptor& descriptor);
    std::unique_ptr<SceneState> owned_state_;

    friend class Renderer;
};

}  // namespace micropixel

#endif
