#ifndef MICROPIXEL_SDK_SCENE_HPP
#define MICROPIXEL_SDK_SCENE_HPP

#include <stdint.h>

namespace micropixel {

class SceneState;
class SceneUpdate;

struct SceneDescriptor final {
    uint32_t logical_width{};
    uint32_t logical_height{};
    Color background{Color::Black()};
};

struct LayerProperties final {
    Rect clip{};
    Point translation{};
    int16_t z_order{};
    uint8_t opacity{255U};
    bool visible{true};
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
    [[nodiscard]] constexpr bool valid() const { return state_ != nullptr; }
    void SetVisible(SceneUpdate& update, bool visible);
    void SetLayer(SceneUpdate& update, Layer layer);

   protected:
    constexpr NodeHandle(SceneState* state, uint16_t id) : state_(state), id_(id) {}
    SceneState* state_{};
    uint16_t id_{};

    friend class Scene;
};

class Layer final {
   public:
    constexpr Layer() = default;
    [[nodiscard]] constexpr bool valid() const { return state_ != nullptr && id_ != 0U; }
    void SetClip(SceneUpdate& update, Rect clip);
    void SetTranslation(SceneUpdate& update, Point translation);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);
    void SetVisible(SceneUpdate& update, bool visible);
    void SetZOrder(SceneUpdate& update, int16_t z_order);

   private:
    constexpr Layer(SceneState* state, uint8_t id) : state_(state), id_(id) {}
    SceneState* state_{};
    uint8_t id_{};

    friend class Scene;
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
    friend class Scene;
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
    friend class Scene;
};

class SurfaceNode final : public NodeHandle {
   public:
    constexpr SurfaceNode() = default;
    void SetDestination(SceneUpdate& update, Rect destination);
    void SetSource(SceneUpdate& update, Rect source);
    void SetOpacity(SceneUpdate& update, uint8_t opacity);

   private:
    using NodeHandle::NodeHandle;
    friend class Scene;
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
    friend class Scene;
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
    constexpr SpriteBatch(SceneState* state, uint16_t id, uint16_t capacity)
        : NodeHandle(state, id), capacity_(capacity) {}
    uint16_t capacity_{};

    friend class Scene;
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
    friend class NodeHandle;
    friend class Layer;
    friend class ShapeNode;
    friend class SpriteNode;
    friend class SpriteBatch;
    friend class LabelNode;
};

class Scene final {
   public:
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&&) = delete;
    ~Scene();

    [[nodiscard]] SceneUpdate BeginUpdate();
    void SetBackground(SceneUpdate& update, Color color);
    [[nodiscard]] Layer CreateLayer(const LayerProperties& properties);
    [[nodiscard]] ShapeNode CreateShape(Rect rect, Color color, Layer layer = Layer{nullptr, 0U},
                                        uint8_t opacity = 255U);
    [[nodiscard]] SpriteNode CreateSprite(const Texture& texture, Rect destination, Rect source,
                                          Layer layer = Layer{nullptr, 0U}, uint8_t opacity = 255U);
    [[nodiscard]] SurfaceNode CreateSurfaceNode(const StreamingTexture& surface, Rect destination, Rect source,
                                                Layer layer = Layer{nullptr, 0U}, uint8_t opacity = 255U);
    [[nodiscard]] SpriteBatch CreateSpriteBatch(const Texture& texture, uint16_t capacity,
                                                Layer layer = Layer{nullptr, 0U}, uint8_t opacity = 255U);
    // A textureless batch is a batch of colored quads and maps directly to
    // accelerated fills. It is the preferred representation for grid games.
    [[nodiscard]] SpriteBatch CreateSpriteBatch(uint16_t capacity, Layer layer = Layer{nullptr, 0U},
                                                uint8_t opacity = 255U);
    [[nodiscard]] LabelNode CreateLabel(Point position, const char* text, Color color,
                                        SystemFont font = SystemFont::kMedium, Layer layer = Layer{nullptr, 0U},
                                        bool centered = false);
    [[nodiscard]] uint16_t node_count() const;

   private:
    struct CapabilityToken {};
    explicit Scene(CapabilityToken, const SceneDescriptor& descriptor);
    [[nodiscard]] SpriteBatch CreateSpriteBatchInternal(uint32_t texture, uint16_t capacity, Layer layer,
                                                        uint8_t opacity);
    SceneState* state_{};

    friend class Renderer;
};

}  // namespace micropixel

#endif
