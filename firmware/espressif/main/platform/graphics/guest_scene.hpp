#pragma once

#include <cstdint>

#include "device/contracts/graphics.hpp"

namespace micropixel::platform::graphics {

enum class GuestSceneNodeKind : uint8_t {
    kRect,
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
    uint8_t layer_id{};
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    uint32_t rgb888{};
    uint8_t opacity{};
    micropixel_texture_handle_t texture{};
    int32_t source_x{};
    int32_t source_y{};
    int32_t source_width{};
    int32_t source_height{};
    micropixel_font_handle_t font{};
    uint16_t text_length{};
    uint16_t batch_capacity{};
    uint16_t batch_instance_offset{};
    char text[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]{};
};

struct GuestSceneLayer final {
    int32_t clip_x{};
    int32_t clip_y{};
    int32_t width{};
    int32_t height{};
    int32_t translate_x{};
    int32_t translate_y{};
    int16_t z_order{};
    uint8_t opacity{255U};
    bool visible{true};
};

// Fixed-capacity authoritative scene. Every message is applied to scratch and
// becomes visible only after the complete message and resulting scene pass
// validation.
class GuestScene final {
   public:
    GuestScene(GuestSceneNode* first, GuestSceneNode* second, uint16_t capacity,
               GuestSceneSpriteInstance* first_instances, GuestSceneSpriteInstance* second_instances,
               uint16_t instance_capacity)
        : current_(first),
          scratch_(second),
          capacity_(capacity),
          current_instances_(first_instances),
          scratch_instances_(second_instances),
          instance_capacity_(instance_capacity) {}

    GuestScene(const GuestScene&) = delete;
    GuestScene& operator=(const GuestScene&) = delete;

    [[nodiscard]] int32_t Apply(const uint8_t* bytes, uint32_t length, int32_t logical_width, int32_t logical_height,
                                device::BitmapResolver bitmap_resolver, void* bitmap_context,
                                device::FontValidator font_validator, void* font_context);
    void Reset();

    [[nodiscard]] const GuestSceneNode* Nodes() const { return current_; }
    [[nodiscard]] const GuestSceneLayer* Layers() const { return layers_; }
    [[nodiscard]] const GuestSceneSpriteInstance* Instances() const { return current_instances_; }
    [[nodiscard]] uint16_t NodeCount() const { return node_count_; }
    [[nodiscard]] uint16_t LayerCount() const { return layer_count_; }
    [[nodiscard]] uint16_t BatchInstanceCount() const { return batch_instance_count_; }
    [[nodiscard]] uint32_t Background() const { return background_rgb888_; }
    [[nodiscard]] uint32_t Generation() const { return generation_; }
    [[nodiscard]] uint32_t Revision() const { return revision_; }
    [[nodiscard]] bool LastApplyWasKeyframe() const { return last_apply_was_keyframe_; }
    [[nodiscard]] bool BackgroundChanged() const { return background_changed_; }
    [[nodiscard]] uint32_t NodeChanges(uint16_t id) const { return id < node_count_ ? node_changes_[id] : 0U; }
    [[nodiscard]] uint32_t LayerChanges(uint8_t id) const { return id <= layer_count_ ? layer_changes_[id] : 0U; }
    [[nodiscard]] uint32_t InstanceChanges(uint16_t id) const {
        return id < batch_instance_count_ ? instance_changes_[id] : 0U;
    }

   private:
    [[nodiscard]] bool ValidateResult(int32_t logical_width, int32_t logical_height,
                                      device::BitmapResolver bitmap_resolver, void* bitmap_context,
                                      device::FontValidator font_validator, void* font_context, uint16_t node_count,
                                      uint16_t layer_count, uint16_t batch_instance_count) const;

    GuestSceneNode* current_{};
    GuestSceneNode* scratch_{};
    uint16_t capacity_{};
    GuestSceneSpriteInstance* current_instances_{};
    GuestSceneSpriteInstance* scratch_instances_{};
    uint16_t instance_capacity_{};
    GuestSceneLayer layers_[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    GuestSceneLayer scratch_layers_[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    uint16_t node_count_{};
    uint16_t layer_count_{};
    uint16_t batch_instance_count_{};
    uint32_t background_rgb888_{};
    uint32_t generation_{};
    uint32_t revision_{};
    uint8_t node_changes_[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    uint8_t layer_changes_[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    uint8_t instance_changes_[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    bool last_apply_was_keyframe_{};
    bool background_changed_{};
    bool valid_{};
};

}  // namespace micropixel::platform::graphics
