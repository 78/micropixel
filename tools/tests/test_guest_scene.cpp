#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "abi/micropixel_abi.h"
#include "platform/graphics/guest_scene.hpp"
#include "runtime/scene_delta.hpp"

namespace graphics = micropixel::platform::graphics;

namespace {

constexpr uint32_t kCommonMask = MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE |
                                 MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY | MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER;
constexpr uint32_t kContentMask = kCommonMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kKind = MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
constexpr uint32_t kBatchMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                                MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT | kKind;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;
constexpr uint32_t kLayerMask = MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP | MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION |
                                MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER;

class Message final {
   public:
    Message(uint16_t kind, uint32_t generation, uint32_t base_revision, uint32_t revision, uint16_t nodes,
            uint16_t layers, uint16_t batch_instances = 0U)
        : kind_(kind),
          generation_(generation),
          base_revision_(base_revision),
          revision_(revision),
          nodes_(nodes),
          layers_(layers),
          batch_instances_(batch_instances) {
        bytes_.resize(sizeof(micropixel_graphics_scene_header_t));
    }

    template <typename Record>
    void Add(const Record& record) {
        const size_t offset = bytes_.size();
        bytes_.resize(offset + sizeof(record));
        std::memcpy(bytes_.data() + offset, &record, sizeof(record));
        ++records_;
    }

    void AddText(uint16_t node_id, uint32_t mask, std::string_view text) {
        const uint32_t size = (sizeof(micropixel_graphics_scene_text_record_t) + text.size() + 3U) & ~3U;
        micropixel_graphics_scene_text_record_t record{
            .node =
                {
                    .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_TEXT, .size = static_cast<uint16_t>(size)},
                    .node_id = node_id,
                    .layer_id = 0U,
                    .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                    .property_mask = mask,
                },
            .x = 4,
            .y = 1,
            .rgb888 = 0xffffffU,
            .font = 1U,
            .text_length = static_cast<uint16_t>(text.size()),
        };
        const size_t offset = bytes_.size();
        bytes_.resize(offset + size, 0U);
        std::memcpy(bytes_.data() + offset, &record, sizeof(record));
        std::memcpy(bytes_.data() + offset + sizeof(record), text.data(), text.size());
        ++records_;
    }

    void AddInstances(uint16_t batch_node_id, uint16_t first, uint32_t mask,
                      const std::vector<micropixel_graphics_scene_sprite_instance_t>& instances) {
        const uint32_t size =
            sizeof(micropixel_graphics_scene_batch_instances_record_t) + instances.size() * sizeof(instances[0]);
        const micropixel_graphics_scene_batch_instances_record_t record{
            .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES, .size = static_cast<uint16_t>(size)},
            .batch_node_id = batch_node_id,
            .first_instance = first,
            .instance_count = static_cast<uint16_t>(instances.size()),
            .reserved0 = 0U,
            .property_mask = mask,
        };
        const size_t offset = bytes_.size();
        bytes_.resize(offset + size);
        std::memcpy(bytes_.data() + offset, &record, sizeof(record));
        std::memcpy(bytes_.data() + offset + sizeof(record), instances.data(), instances.size() * sizeof(instances[0]));
        ++records_;
    }

    const std::vector<uint8_t>& Finish() {
        const micropixel_graphics_scene_header_t header{
            .magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC,
            .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
            .interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR,
            .kind = kind_,
            .flags = 0U,
            .total_size = static_cast<uint32_t>(bytes_.size()),
            .generation = generation_,
            .base_revision = base_revision_,
            .revision = revision_,
            .record_count = records_,
            .node_count = nodes_,
            .layer_count = layers_,
            .batch_instance_count = batch_instances_,
        };
        std::memcpy(bytes_.data(), &header, sizeof(header));
        return bytes_;
    }

   private:
    std::vector<uint8_t> bytes_{};
    uint16_t kind_{};
    uint32_t generation_{};
    uint32_t base_revision_{};
    uint32_t revision_{};
    uint16_t nodes_{};
    uint16_t layers_{};
    uint16_t batch_instances_{};
    uint16_t records_{};
};

micropixel_graphics_scene_background_record_t Background(uint32_t color) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND,
                   .size = sizeof(micropixel_graphics_scene_background_record_t)},
        .property_mask = MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR,
        .rgb888 = color,
    };
}

micropixel_graphics_scene_layer_record_t Layer(int32_t translate_x, uint32_t mask = kLayerMask) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_LAYER,
                   .size = sizeof(micropixel_graphics_scene_layer_record_t)},
        .layer_id = 1U,
        .reserved0 = 0U,
        .property_mask = mask,
        .clip_x = 1,
        .clip_y = 0,
        .width = 6,
        .height = 4,
        .translate_x = translate_x,
        .translate_y = 0,
        .z_order = 0,
        .opacity = 255U,
        .visible = 1U,
    };
}

micropixel_graphics_scene_rect_record_t Rect(int32_t x, uint32_t mask = kCommonMask | kKind, uint8_t layer_id = 1U) {
    return {
        .node =
            {
                .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_RECT,
                           .size = sizeof(micropixel_graphics_scene_rect_record_t)},
                .node_id = 0U,
                .layer_id = layer_id,
                .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                .property_mask = mask,
            },
        .x = x,
        .y = 1,
        .width = 2,
        .height = 2,
        .rgb888 = 0x00ff00U,
        .opacity = 255U,
        .reserved0 = {},
    };
}

bool ResolveBitmap(void*, micropixel_texture_handle_t texture, micropixel::device::BitmapView& view) {
    static const std::array<uint8_t, 12U> pixels{};
    if (texture != 7U) {
        return false;
    }
    view = {.data = pixels.data(),
            .size = static_cast<uint32_t>(pixels.size()),
            .width = 2U,
            .height = 2U,
            .stride = 6U,
            .pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888,
            .flags = 0U};
    return true;
}

bool ValidateFont(void*, micropixel_font_handle_t font) { return font == 1U; }

void KeyframeAndPatchesAreAtomicAndRevisioned() {
    std::array<graphics::GuestSceneNode, 4U> first{};
    std::array<graphics::GuestSceneNode, 4U> second{};
    std::array<graphics::GuestSceneSpriteInstance, 8U> first_instances{};
    std::array<graphics::GuestSceneSpriteInstance, 8U> second_instances{};
    graphics::GuestScene scene(first.data(), second.data(), static_cast<uint16_t>(first.size()), first_instances.data(),
                               second_instances.data(), static_cast<uint16_t>(first_instances.size()));

    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 11U, 0U, 1U, 2U, 1U);
    keyframe.Add(Background(0x010203U));
    keyframe.Add(Layer(0));
    keyframe.Add(Rect(2));
    keyframe.AddText(1U, kContentMask | kKind, "GO");
    const auto& keyframe_bytes = keyframe.Finish();
    const int32_t keyframe_status = scene.Apply(keyframe_bytes.data(), static_cast<uint32_t>(keyframe_bytes.size()), 8,
                                                4, ResolveBitmap, nullptr, ValidateFont, nullptr);
    assert(keyframe_status == MICROPIXEL_STATUS_OK);
    assert(scene.Generation() == 11U && scene.Revision() == 1U);
    assert(scene.NodeCount() == 2U && scene.LayerCount() == 1U);
    assert(scene.Nodes()[0].x == 2 && scene.Nodes()[1].text_length == 2U);

    Message patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 11U, 1U, 2U, 2U, 1U);
    patch.Add(Layer(1, MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION));
    patch.Add(Rect(5, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY));
    const auto& patch_bytes = patch.Finish();
    assert(scene.Apply(patch_bytes.data(), static_cast<uint32_t>(patch_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Revision() == 2U && scene.Layers()[1].translate_x == 1);
    assert(scene.Nodes()[0].x == 5 && scene.Nodes()[0].rgb888 == 0x00ff00U);
    assert(scene.Nodes()[1].text_length == 2U);

    Message stale(MICROPIXEL_GRAPHICS_SCENE_PATCH, 11U, 1U, 3U, 2U, 1U);
    stale.Add(Rect(1, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY));
    const auto& stale_bytes = stale.Finish();
    assert(scene.Apply(stale_bytes.data(), static_cast<uint32_t>(stale_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_STALE_STATE);
    assert(scene.Revision() == 2U && scene.Nodes()[0].x == 5);

    Message invalid(MICROPIXEL_GRAPHICS_SCENE_PATCH, 11U, 2U, 3U, 2U, 1U);
    invalid.Add(Rect(20, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY));
    const auto& invalid_bytes = invalid.Finish();
    assert(scene.Apply(invalid_bytes.data(), static_cast<uint32_t>(invalid_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(scene.Revision() == 2U && scene.Nodes()[0].x == 5);
}

void TextureReplacementRequiresACompleteKindChange() {
    std::array<graphics::GuestSceneNode, 2U> first{};
    std::array<graphics::GuestSceneNode, 2U> second{};
    std::array<graphics::GuestSceneSpriteInstance, 2U> first_instances{};
    std::array<graphics::GuestSceneSpriteInstance, 2U> second_instances{};
    graphics::GuestScene scene(first.data(), second.data(), static_cast<uint16_t>(first.size()), first_instances.data(),
                               second_instances.data(), static_cast<uint16_t>(first_instances.size()));
    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 0U);
    keyframe.Add(Background(0U));
    keyframe.Add(Rect(1, kCommonMask | kKind, 0U));
    const auto& keyframe_bytes = keyframe.Finish();
    const int32_t replacement_keyframe_status =
        scene.Apply(keyframe_bytes.data(), static_cast<uint32_t>(keyframe_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                    ValidateFont, nullptr);
    assert(replacement_keyframe_status == MICROPIXEL_STATUS_OK);

    micropixel_graphics_scene_texture_record_t texture{
        .node =
            {
                .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE,
                           .size = sizeof(micropixel_graphics_scene_texture_record_t)},
                .node_id = 0U,
                .layer_id = 0U,
                .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                .property_mask = kContentMask | kKind,
            },
        .x = 2,
        .y = 1,
        .width = 2,
        .height = 2,
        .texture = 7U,
        .source_x = 0,
        .source_y = 0,
        .source_width = 2,
        .source_height = 2,
        .opacity = 255U,
        .reserved0 = {},
    };
    Message patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 1U, 1U, 2U, 1U, 0U);
    patch.Add(texture);
    const auto& patch_bytes = patch.Finish();
    assert(scene.Apply(patch_bytes.data(), static_cast<uint32_t>(patch_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Nodes()[0].kind == graphics::GuestSceneNodeKind::kTexture && scene.Nodes()[0].texture == 7U);
}

void SpriteBatchInstancesPatchIndependently() {
    std::array<graphics::GuestSceneNode, 2U> first{};
    std::array<graphics::GuestSceneNode, 2U> second{};
    std::array<graphics::GuestSceneSpriteInstance, 4U> first_instances{};
    std::array<graphics::GuestSceneSpriteInstance, 4U> second_instances{};
    graphics::GuestScene scene(first.data(), second.data(), static_cast<uint16_t>(first.size()), first_instances.data(),
                               second_instances.data(), static_cast<uint16_t>(first_instances.size()));

    const micropixel_graphics_scene_sprite_batch_record_t batch{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
                            .size = sizeof(micropixel_graphics_scene_sprite_batch_record_t)},
                 .node_id = 0U,
                 .layer_id = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask = kBatchMask},
        .texture = 7U,
        .capacity = 2U,
        .opacity = 255U,
        .reserved0 = 0U,
    };
    std::vector<micropixel_graphics_scene_sprite_instance_t> instances(2U);
    for (uint16_t index = 0U; index < instances.size(); ++index) {
        instances[index] = {.x = static_cast<int32_t>(index * 2U),
                            .y = 0,
                            .width = 2,
                            .height = 2,
                            .source_x = 0,
                            .source_y = 0,
                            .source_width = 2,
                            .source_height = 2,
                            .opacity = 255U,
                            .flags = MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE,
                            .reserved0 = 0U};
    }
    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 3U, 0U, 1U, 1U, 0U, 2U);
    keyframe.Add(Background(0U));
    keyframe.Add(batch);
    keyframe.AddInstances(0U, 0U, kInstanceMask, instances);
    const auto& keyframe_bytes = keyframe.Finish();
    assert(scene.Apply(keyframe_bytes.data(), static_cast<uint32_t>(keyframe_bytes.size()), 8, 4, ResolveBitmap,
                       nullptr, ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.BatchInstanceCount() == 2U && scene.Instances()[1].x == 2);

    instances.resize(1U);
    instances[0].x = 5;
    Message patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 3U, 1U, 2U, 1U, 0U, 2U);
    patch.AddInstances(0U, 1U, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY, instances);
    const auto& patch_bytes = patch.Finish();
    assert(scene.Apply(patch_bytes.data(), static_cast<uint32_t>(patch_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Instances()[0].x == 0 && scene.Instances()[1].x == 5);
}

void SceneTransactionsSerializeNetPropertyChanges() {
    constexpr uint32_t kDirtyGeometry = 1U << 0U;
    constexpr uint32_t kDirtyVisibility = 1U << 1U;

    uint32_t dirty = 0U;
    micropixel::detail::UpdateScenePropertyDirty(dirty, 0U, kDirtyVisibility, true);
    assert(dirty == kDirtyVisibility);
    micropixel::detail::UpdateScenePropertyDirty(dirty, 0U, kDirtyVisibility, false);
    assert(dirty == 0U);

    dirty = kDirtyVisibility;
    micropixel::detail::UpdateScenePropertyDirty(dirty, kDirtyVisibility, kDirtyVisibility, true);
    micropixel::detail::UpdateScenePropertyDirty(dirty, kDirtyVisibility, kDirtyVisibility, false);
    assert(dirty == kDirtyVisibility);

    dirty = 0U;
    micropixel::detail::UpdateScenePropertyDirty(dirty, 0U, kDirtyVisibility, true);
    micropixel::detail::UpdateScenePropertyDirty(dirty, 0U, kDirtyGeometry, true);
    micropixel::detail::UpdateScenePropertyDirty(dirty, 0U, kDirtyVisibility, false);
    assert(dirty == kDirtyGeometry);
}

}  // namespace

int main() {
    KeyframeAndPatchesAreAtomicAndRevisioned();
    TextureReplacementRequiresACompleteKindChange();
    SpriteBatchInstancesPatchIndependently();
    SceneTransactionsSerializeNetPropertyChanges();
    return 0;
}
