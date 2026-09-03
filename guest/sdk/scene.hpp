#ifndef MICROPIXEL_SDK_SCENE_HPP
#define MICROPIXEL_SDK_SCENE_HPP

#include <stdint.h>

namespace micropixel {

class SceneState;
class SceneUpdate;
class Renderer;
class Container;
class ContainerNode;
class ShapeNode;
class RoundedRectNode;
class SpriteNode;
class SurfaceNode;
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
    // Hint that this subtree changes rarely compared with how often it is
    // translated (a scrolling map or tile layer). The Host may rasterize the
    // subtree once, in the container's local coordinates, into a retained cache
    // and re-composite that cache on every translation: content changes stay
    // expensive, translation becomes cheap. The cache is composited as an
    // opaque layer whose uncovered pixels show the Scene background color, so
    // nothing drawn below this container in draw order shows through it. Give
    // the container an explicit clip; it bounds the cache.
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
    // Terminal on a successful Present(). If the SceneUpdate rolls back, the
    // handle becomes valid again. Repeated destruction of a stale handle is a
    // no-op.
    void Destroy(SceneUpdate& update);
    void SetVisible(SceneUpdate& update, bool visible);

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

    [[nodiscard]] ContainerNode CreateContainer(const ContainerProperties& properties = {});
    [[nodiscard]] ShapeNode CreateShape(Rect rect, Color color, uint8_t opacity = 255U);
    [[nodiscard]] RoundedRectNode CreateRoundedRect(Rect rect, const RoundedRectStyle& style);
    [[nodiscard]] SpriteNode CreateSprite(const Texture& texture, Rect destination, Rect source,
                                          uint8_t opacity = 255U);
    [[nodiscard]] SurfaceNode CreateSurfaceNode(const StreamingTexture& surface, Rect destination, Rect source,
                                                uint8_t opacity = 255U);
    [[nodiscard]] SpriteBatch CreateSpriteBatch(const Texture& texture, uint16_t capacity, uint8_t opacity = 255U);
    // A textureless batch is a batch of colored quads and maps directly to
    // accelerated fills. It is the preferred representation for grid games.
    [[nodiscard]] SpriteBatch CreateSpriteBatch(uint16_t capacity, uint8_t opacity = 255U);
    [[nodiscard]] LabelNode CreateLabel(Point position, const char* text, Color color,
                                        SystemFont font = SystemFont::kMedium, bool centered = false);
    [[nodiscard]] ui::ImageButton CreateImageButton(const Texture& texture,
                                                    const ui::ImageButtonProperties& properties);
    [[nodiscard]] ui::FlexContainer CreateFlexContainer(const ui::FlexContainerProperties& properties);
    [[nodiscard]] ui::GridContainer CreateGridContainer(const ui::GridContainerProperties& properties);
    [[nodiscard]] ui::TextButton CreateTextButton(const ui::TextButtonProperties& properties);

   protected:
    constexpr Container(SceneState* state, uint16_t id, uint32_t generation)
        : state_(state), id_(id), generation_(generation) {}
    [[nodiscard]] SpriteBatch CreateSpriteBatchInternal(uint32_t texture, uint16_t capacity, uint8_t opacity);
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
    void Destroy(SceneUpdate& update);
    void SetClip(SceneUpdate& update, Rect clip);
    void SetTranslation(SceneUpdate& update, Point translation);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);
    void SetVisible(SceneUpdate& update, bool visible);
    void SetZOrder(SceneUpdate& update, int16_t z_order);
    // See ContainerProperties::cache_content.
    void SetCacheContent(SceneUpdate& update, bool cache_content);

   private:
    constexpr ContainerNode(SceneState* state, uint16_t id, uint32_t generation) : Container(state, id, generation) {}

    friend class Container;
    friend class SceneState;
    friend class NodeHandle;
};

class ShapeNode final : public NodeHandle {
   public:
    constexpr ShapeNode() = default;
    void SetRect(SceneUpdate& update, Rect rect);
    void SetColor(SceneUpdate& update, Color color);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class RoundedRectNode final : public NodeHandle {
   public:
    constexpr RoundedRectNode() = default;
    void SetRect(SceneUpdate& update, Rect rect);
    void SetFillColor(SceneUpdate& update, Color color);
    void SetStrokeColor(SceneUpdate& update, Color color);
    void SetRadius(SceneUpdate& update, uint32_t radius);
    void SetStrokeWidth(SceneUpdate& update, uint32_t stroke_width);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class SpriteNode final : public NodeHandle {
   public:
    constexpr SpriteNode() = default;
    void SetDestination(SceneUpdate& update, Rect destination);
    void SetSource(SceneUpdate& update, Rect source);
    void SetTexture(SceneUpdate& update, const Texture& texture);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class SurfaceNode final : public NodeHandle {
   public:
    constexpr SurfaceNode() = default;
    void SetDestination(SceneUpdate& update, Rect destination);
    void SetSource(SceneUpdate& update, Rect source);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class LabelNode final : public NodeHandle {
   public:
    constexpr LabelNode() = default;
    void SetPosition(SceneUpdate& update, Point position);
    void SetText(SceneUpdate& update, const char* text);
    void SetColor(SceneUpdate& update, Color color);
    void SetFont(SceneUpdate& update, SystemFont font);
    void SetCentered(SceneUpdate& update, bool centered);

   private:
    using NodeHandle::NodeHandle;
    friend class Container;
};

class SpriteBatch final : public NodeHandle {
   public:
    constexpr SpriteBatch() = default;
    [[nodiscard]] constexpr uint16_t capacity() const { return capacity_; }
    void SetTexture(SceneUpdate& update, const Texture& texture);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);
    void SetInstance(SceneUpdate& update, uint16_t instance_id, const SpriteInstance& instance);
    void SetInstanceVisible(SceneUpdate& update, uint16_t instance_id, bool visible);

   private:
    constexpr SpriteBatch(SceneState* state, uint16_t id, uint32_t generation, uint16_t capacity)
        : NodeHandle(state, id, generation), capacity_(capacity) {}
    uint16_t capacity_{};

    friend class Container;
};

class SceneUpdate final {
   public:
    SceneUpdate(const SceneUpdate&) = delete;
    SceneUpdate& operator=(const SceneUpdate&) = delete;
    SceneUpdate(SceneUpdate&& other) noexcept;
    SceneUpdate& operator=(SceneUpdate&&) = delete;
    ~SceneUpdate();

    [[nodiscard]] Result<void> Present();
    [[nodiscard]] constexpr bool active_for(const SceneState* state) const { return active_ && state_ == state; }

   private:
    explicit SceneUpdate(SceneState* state) : state_(state), active_(true) {}
    SceneState* state_{};
    bool active_{};

    friend class Scene;
    friend class Container;
    friend class NodeHandle;
    friend class ContainerNode;
    friend class ShapeNode;
    friend class RoundedRectNode;
    friend class SpriteNode;
    friend class SpriteBatch;
    friend class LabelNode;
};

class Scene final : public Container {
   public:
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&&) = delete;
    ~Scene();

    [[nodiscard]] SceneUpdate BeginUpdate();
    template <typename Function>
    [[nodiscard]] Result<void> Update(Function&& function) {
        auto update = BeginUpdate();
        static_cast<Function&&>(function)(update);
        return update.Present();
    }
    void SetBackground(SceneUpdate& update, Color color);
    [[nodiscard]] uint16_t node_count() const;

   private:
    struct CapabilityToken {};
    explicit Scene(CapabilityToken, const SceneDescriptor& descriptor);

    friend class Renderer;
};

}  // namespace micropixel

#endif
