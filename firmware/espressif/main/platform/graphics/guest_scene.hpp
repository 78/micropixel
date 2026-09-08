#pragma once

#include <cstdint>
#include <span>

#include "device/contracts/graphics.hpp"

namespace micropixel::platform::graphics {

enum class GuestSceneNodeKind : uint8_t {
    kRect,
    kRoundedRect,
    kTexture,
    kText,
    kSpriteBatch,
};

struct GuestSceneSpriteInstance final {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    int32_t source_x{};
    int32_t source_y{};
    int32_t source_width{};
    int32_t source_height{};
    uint32_t rgb888{};
    uint8_t opacity{};
    uint8_t flags{};
};

struct GuestSceneNode final {
    GuestSceneNodeKind kind{};
    bool visible{};
    bool text_centered{};
    uint16_t parent_container_id{};
    uint16_t sibling_order{};
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    uint32_t rgb888{};
    uint32_t stroke_rgb888{};
    uint32_t radius{};
    uint32_t stroke_width{};
    uint8_t opacity{};
    micropixel_texture_handle_t texture_handle{};
    int32_t source_x{};
    int32_t source_y{};
    int32_t source_width{};
    int32_t source_height{};
    micropixel_font_handle_t font_handle{};
    uint16_t text_length{};
    uint16_t batch_capacity{};
    uint16_t batch_instance_offset{};
    // Offset of text_length bytes plus a terminating NUL in the scene text
    // arena (GuestScene::Text). Stable until the owner compacts the arena.
    uint32_t text_offset{};
};

struct GuestSceneContainer final {
    uint16_t parent_container_id{};
    uint16_t sibling_order{};
    int32_t clip_x{};
    int32_t clip_y{};
    int32_t width{};
    int32_t height{};
    int32_t translate_x{};
    int32_t translate_y{};
    int16_t z_order{};
    uint8_t opacity{255U};
    bool visible{true};
    // Graphics 1.4 CACHED_CONTENT hint; see micropixel_graphics_scene_container_flag_t.
    bool cached_content{};
};

// All spans hold two halves (current + scratch) except node_marks,
// instance_marks and container_marks, which are per-Apply validation scratch,
// and text, which is a single persistent arena.
struct GuestSceneStorageView final {
    std::span<GuestSceneNode> nodes;
    std::span<GuestSceneSpriteInstance> instances;
    std::span<GuestSceneContainer> containers;  // 2 * (container capacity + 1)
    std::span<uint16_t> draw_order;
    std::span<uint8_t> node_changes;
    std::span<uint8_t> instance_changes;
    std::span<uint8_t> container_changes;  // container capacity + 1
    std::span<uint8_t> node_marks;         // node capacity
    std::span<uint8_t> instance_marks;     // instance capacity
    std::span<uint8_t> container_marks;    // container capacity + 1
    std::span<char> text;
};

// Authoritative scene: Apply never allocates and commits only a valid message.
class GuestScene final {
   public:
    explicit GuestScene(GuestSceneStorageView storage) { RebindStorage(storage); }
    // Called only between submissions with equal or larger, disjoint storage.
    // Copies the committed state before the owner releases the old allocation
    // and compacts the text arena, so retained text pointers become invalid.
    void RebindStorage(GuestSceneStorageView storage);
    // Moves live node text to the other arena half. Text pointers taken from
    // the committed scene stay readable until the following compaction.
    void CompactText();

    GuestScene(const GuestScene&) = delete;
    GuestScene& operator=(const GuestScene&) = delete;

    [[nodiscard]] int32_t Apply(const uint8_t* bytes, uint32_t length, int32_t logical_width, int32_t logical_height,
                                device::BitmapResolver bitmap_resolver, void* bitmap_context,
                                device::FontValidator font_validator, void* font_context);
    void Reset();

    [[nodiscard]] const GuestSceneNode* Nodes() const { return current_; }
    [[nodiscard]] const GuestSceneContainer* Containers() const { return containers_; }
    [[nodiscard]] const GuestSceneSpriteInstance* Instances() const { return current_instances_; }
    [[nodiscard]] uint16_t NodeCount() const { return node_count_; }
    [[nodiscard]] uint16_t ContainerCount() const { return container_count_; }
    [[nodiscard]] uint16_t BatchInstanceCount() const { return batch_instance_count_; }
    [[nodiscard]] uint16_t NodeCapacity() const { return capacity_; }
    [[nodiscard]] uint16_t ContainerCapacity() const { return container_capacity_; }
    [[nodiscard]] uint16_t InstanceCapacity() const { return instance_capacity_; }
    // NUL-terminated text of a text node.
    [[nodiscard]] const char* Text(const GuestSceneNode& node) const { return text_ + node.text_offset; }
    // Bytes consumed in the active arena half (including garbage from replaced
    // text), bytes still referenced by committed nodes, and the half's
    // capacity. Apply appends at most the message's text bytes plus one NUL per
    // TEXT record, so the owner guarantees TextUsed() + that <= TextCapacity()
    // beforehand (compacting or growing as needed).
    [[nodiscard]] uint32_t TextUsed() const { return text_used_; }
    [[nodiscard]] uint32_t TextLive() const;
    [[nodiscard]] uint32_t TextCapacity() const { return text_capacity_; }
    [[nodiscard]] uint32_t Background() const { return background_rgb888_; }
    [[nodiscard]] uint32_t Generation() const { return generation_; }
    [[nodiscard]] uint32_t Revision() const { return revision_; }
    [[nodiscard]] bool LastApplyWasKeyframe() const { return last_apply_was_keyframe_; }
    [[nodiscard]] bool BackgroundChanged() const { return background_changed_; }
    [[nodiscard]] uint32_t NodeChanges(uint16_t id) const { return id < node_count_ ? node_changes_[id] : 0U; }
    [[nodiscard]] uint32_t ContainerChanges(uint16_t id) const {
        return id <= container_count_ ? container_changes_[id] : 0U;
    }
    [[nodiscard]] uint32_t AncestorChanges(uint16_t container_id) const;
    [[nodiscard]] uint16_t DrawNodeId(uint16_t order) const { return draw_node_order_[order]; }
    [[nodiscard]] bool TreeOrderChanged() const { return tree_order_changed_; }
    [[nodiscard]] uint32_t InstanceChanges(uint16_t id) const {
        return id < batch_instance_count_ ? instance_changes_[id] : 0U;
    }

   private:
    [[nodiscard]] bool ValidateResult(device::BitmapResolver bitmap_resolver, void* bitmap_context,
                                      device::FontValidator font_validator, void* font_context, uint16_t node_count,
                                      uint16_t container_count, uint16_t batch_instance_count);
    [[nodiscard]] bool BuildDrawOrder(uint16_t node_count, uint16_t container_count);
    [[nodiscard]] bool AppendChildren(uint16_t parent_id, uint16_t node_count, uint16_t container_count,
                                      uint16_t& output_count);

    GuestSceneNode* current_{};
    GuestSceneNode* scratch_{};
    uint16_t capacity_{};
    GuestSceneSpriteInstance* current_instances_{};
    GuestSceneSpriteInstance* scratch_instances_{};
    uint16_t instance_capacity_{};
    GuestSceneContainer* containers_{};
    GuestSceneContainer* scratch_containers_{};
    uint16_t container_capacity_{};
    uint16_t node_count_{};
    uint16_t container_count_{};
    uint16_t batch_instance_count_{};
    uint32_t background_rgb888_{};
    uint32_t generation_{};
    uint32_t revision_{};
    uint8_t* node_changes_{};
    uint8_t* container_changes_{};
    uint8_t* node_marks_{};
    uint8_t* instance_marks_{};
    uint8_t* container_marks_{};
    uint16_t* draw_node_order_{};
    uint16_t* scratch_draw_node_order_{};
    uint8_t* instance_changes_{};
    char* text_arena_{};  // Two halves of text_capacity_ bytes; text_ is the active one.
    char* text_{};
    uint32_t text_used_{};
    uint32_t text_capacity_{};
    bool last_apply_was_keyframe_{};
    bool background_changed_{};
    bool tree_order_changed_{};
    bool valid_{};
};

}  // namespace micropixel::platform::graphics
