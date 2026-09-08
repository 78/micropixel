#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "abi/micropixel_abi.h"
#include "device/contracts/graphics.hpp"
#include "platform/graphics/guest_scene.hpp"
#include "runtime/scene_delta.hpp"

namespace graphics = micropixel::platform::graphics;

namespace {

constexpr uint32_t kCommonMask = MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE |
                                 MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY;
constexpr uint32_t kContentMask = kCommonMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
constexpr uint32_t kKind = MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
constexpr uint32_t kBatchMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                                MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT | kKind;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;
constexpr uint32_t kViewportMask =
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS;
constexpr uint32_t kContainerMask =
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS;

class Message final {
   public:
    Message(uint16_t kind, uint32_t generation, uint32_t base_revision, uint32_t revision, uint16_t nodes,
            uint16_t containers, uint16_t batch_instances = 0U)
        : kind_(kind),
          generation_(generation),
          base_revision_(base_revision),
          revision_(revision),
          nodes_(nodes),
          containers_(containers),
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
                    .reserved0 = 0U,
                    .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                    .property_mask = mask,
                },
            .x = 4,
            .y = 1,
            .rgb888 = 0xffffffU,
            .font_handle = 1U,
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
        if (kind_ == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME) {
            std::vector<bool> linked(nodes_, false);
            for (size_t offset = sizeof(micropixel_graphics_scene_header_t); offset < bytes_.size();) {
                micropixel_graphics_scene_record_header_t record{};
                std::memcpy(&record, bytes_.data() + offset, sizeof(record));
                if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK) {
                    micropixel_graphics_scene_node_link_record_t link{};
                    std::memcpy(&link, bytes_.data() + offset, sizeof(link));
                    if (link.node_id < nodes_) linked[link.node_id] = true;
                }
                offset += record.size;
            }
            for (uint16_t node = 0; node < nodes_; ++node) {
                if (linked[node]) continue;
                Add(micropixel_graphics_scene_node_link_record_t{
                    .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK,
                               .size = sizeof(micropixel_graphics_scene_node_link_record_t)},
                    .node_id = node,
                    .parent_container_id = 0U,
                    .sibling_order = static_cast<uint16_t>(node + 10U)});
            }
        }
        const micropixel_graphics_scene_header_t header{
            .magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC,
            .kind = kind_,
            .flags = 0U,
            .total_size = static_cast<uint32_t>(bytes_.size()),
            .generation = generation_,
            .base_revision = base_revision_,
            .revision = revision_,
            .record_count = records_,
            .node_count = nodes_,
            .container_count = containers_,
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
    uint16_t containers_{};
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

micropixel_graphics_scene_container_record_t ViewportContainer(int32_t translate_x, uint32_t mask = kViewportMask) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER,
                   .size = sizeof(micropixel_graphics_scene_container_record_t)},
        .container_id = 1U,
        .parent_container_id = 0U,
        .property_mask = mask,
        .clip_x = 1,
        .clip_y = 0,
        .clip_width = 6,
        .clip_height = 4,
        .translate_x = translate_x,
        .translate_y = 0,
        .z_order = 0,
        .opacity = 255U,
        .visible = 1U,
        .sibling_order = 0U,
        .flags = 0U,
    };
}

micropixel_graphics_scene_rect_record_t Rect(int32_t x, uint32_t mask = kCommonMask | kKind) {
    return {
        .node =
            {
                .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_RECT,
                           .size = sizeof(micropixel_graphics_scene_rect_record_t)},
                .node_id = 0U,
                .reserved0 = 0U,
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

micropixel_graphics_scene_container_record_t Container(uint16_t id, uint16_t parent, uint16_t sibling_order,
                                                       int32_t translate_x = 0, uint32_t mask = kContainerMask) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER,
                   .size = sizeof(micropixel_graphics_scene_container_record_t)},
        .container_id = id,
        .parent_container_id = parent,
        .property_mask = mask,
        .clip_x = id == 1U ? 0 : 0,
        .clip_y = 0,
        .clip_width = id == 1U ? 8 : 0,
        .clip_height = id == 1U ? 4 : 0,
        .translate_x = translate_x,
        .translate_y = 0,
        .z_order = 0,
        .opacity = 255U,
        .visible = 1U,
        .sibling_order = sibling_order,
        .flags = 0U,
    };
}

micropixel_graphics_scene_node_link_record_t Link(uint16_t node, uint16_t parent, uint16_t sibling_order) {
    return {
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK,
                   .size = sizeof(micropixel_graphics_scene_node_link_record_t)},
        .node_id = node,
        .parent_container_id = parent,
        .sibling_order = sibling_order,
        .reserved0 = 0U,
    };
}

bool ResolveBitmap(void*, micropixel_texture_handle_t texture_handle, micropixel::device::BitmapView& view) {
    static const std::array<uint8_t, 12U> pixels{};
    if (texture_handle == 8U) {
        view = {.data = pixels.data(),
                .size = 128U * 43U * 3U,
                .width = 128U,
                .height = 43U,
                .stride = 128U * 3U,
                .pixel_format = MICROPIXEL_PIXEL_FORMAT_BGR888,
                .flags = 0U};
        return true;
    }
    if (texture_handle != 7U) {
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

bool ValidateFont(void*, micropixel_font_handle_t font_handle) { return font_handle == 1U; }

template <size_t NodeCapacity, size_t InstanceCapacity, size_t ContainerCapacity = 64U, size_t TextCapacity = 512U>
struct SceneStorage final {
    std::array<graphics::GuestSceneNode, 2U * NodeCapacity> nodes{};
    std::array<graphics::GuestSceneSpriteInstance, 2U * InstanceCapacity> instances{};
    std::array<graphics::GuestSceneContainer, 2U * (ContainerCapacity + 1U)> containers{};
    std::array<uint16_t, 2U * NodeCapacity> order{};
    std::array<uint8_t, NodeCapacity> node_changes{};
    std::array<uint8_t, InstanceCapacity> instance_changes{};
    std::array<uint8_t, ContainerCapacity + 1U> container_changes{};
    std::array<uint8_t, NodeCapacity> node_marks{};
    std::array<uint8_t, InstanceCapacity> instance_marks{};
    std::array<uint8_t, ContainerCapacity + 1U> container_marks{};
    std::array<char, 2U * TextCapacity> text{};
    graphics::GuestScene scene{{nodes, instances, containers, order, node_changes, instance_changes, container_changes,
                                node_marks, instance_marks, container_marks, text}};
};

void KeyframeAndPatchesAreAtomicAndRevisioned() {
    SceneStorage<4U, 8U> storage;
    graphics::GuestScene& scene = storage.scene;

    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 11U, 0U, 1U, 2U, 1U);
    keyframe.Add(Background(0x010203U));
    keyframe.Add(ViewportContainer(0));
    keyframe.Add(Rect(2));
    keyframe.Add(Link(0U, 1U, 10U));
    keyframe.AddText(1U, kContentMask | kKind, "GO");
    const auto& keyframe_bytes = keyframe.Finish();
    const int32_t keyframe_status = scene.Apply(keyframe_bytes.data(), static_cast<uint32_t>(keyframe_bytes.size()), 8,
                                                4, ResolveBitmap, nullptr, ValidateFont, nullptr);
    assert(keyframe_status == MICROPIXEL_STATUS_OK);
    assert(scene.Generation() == 11U && scene.Revision() == 1U);
    assert(scene.NodeCount() == 2U && scene.ContainerCount() == 1U);
    assert(scene.Nodes()[0].x == 2 && scene.Nodes()[1].text_length == 2U);

    Message patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 11U, 1U, 2U, 2U, 1U);
    patch.Add(ViewportContainer(1, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION));
    patch.Add(Rect(5, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY));
    const auto& patch_bytes = patch.Finish();
    assert(scene.Apply(patch_bytes.data(), static_cast<uint32_t>(patch_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Revision() == 2U && scene.Containers()[1].translate_x == 1);
    assert(scene.Nodes()[0].x == 5 && scene.Nodes()[0].rgb888 == 0x00ff00U);
    assert(scene.Nodes()[1].text_length == 2U);

    Message stale(MICROPIXEL_GRAPHICS_SCENE_PATCH, 11U, 1U, 3U, 2U, 1U);
    stale.Add(Rect(1, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY));
    const auto& stale_bytes = stale.Finish();
    assert(scene.Apply(stale_bytes.data(), static_cast<uint32_t>(stale_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_STALE_STATE);
    assert(scene.Revision() == 2U && scene.Nodes()[0].x == 5);

    Message invalid(MICROPIXEL_GRAPHICS_SCENE_PATCH, 11U, 2U, 3U, 2U, 1U);
    auto invalid_rect = Rect(2, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY);
    invalid_rect.width = 0;
    invalid.Add(invalid_rect);
    const auto& invalid_bytes = invalid.Finish();
    assert(scene.Apply(invalid_bytes.data(), static_cast<uint32_t>(invalid_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(scene.Revision() == 2U && scene.Nodes()[0].x == 5);
}

void WideViewportTranslationIsAcceptedButRemainsCanvasBounded() {
    SceneStorage<2U, 2U> storage;
    graphics::GuestScene& scene = storage.scene;

    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 21U, 0U, 1U, 1U, 1U);
    keyframe.Add(Background(0U));
    keyframe.Add(ViewportContainer(120));
    keyframe.Add(Rect(2));
    keyframe.Add(Link(0U, 1U, 10U));
    const auto& keyframe_bytes = keyframe.Finish();
    assert(scene.Apply(keyframe_bytes.data(), static_cast<uint32_t>(keyframe_bytes.size()), 960, 4, ResolveBitmap,
                       nullptr, ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Containers()[1].translate_x == 120);

    Message invalid(MICROPIXEL_GRAPHICS_SCENE_PATCH, 21U, 1U, 2U, 1U, 1U);
    auto bad_container = ViewportContainer(954, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP);
    bad_container.clip_width = -1;
    invalid.Add(bad_container);
    const auto& invalid_bytes = invalid.Finish();
    assert(scene.Apply(invalid_bytes.data(), static_cast<uint32_t>(invalid_bytes.size()), 960, 4, ResolveBitmap,
                       nullptr, ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(scene.Revision() == 1U && scene.Containers()[1].translate_x == 120);
}

void SmallerKeyframeRemovesOldNodes() {
    SceneStorage<4U, 4U> storage;
    graphics::GuestScene& scene = storage.scene;

    Message initial(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 31U, 0U, 1U, 3U, 0U);
    initial.Add(Background(0U));
    auto first_rect = Rect(1, kCommonMask | kKind);
    auto second_rect = Rect(2, kCommonMask | kKind);
    auto third_rect = Rect(3, kCommonMask | kKind);
    second_rect.node.node_id = 1U;
    third_rect.node.node_id = 2U;
    initial.Add(first_rect);
    initial.Add(second_rect);
    initial.Add(third_rect);
    const auto& initial_bytes = initial.Finish();
    assert(scene.Apply(initial_bytes.data(), static_cast<uint32_t>(initial_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.NodeCount() == 3U);

    Message smaller(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 32U, 0U, 1U, 1U, 0U);
    smaller.Add(Background(0U));
    smaller.Add(Rect(4, kCommonMask | kKind));
    const auto& smaller_bytes = smaller.Finish();
    assert(scene.Apply(smaller_bytes.data(), static_cast<uint32_t>(smaller_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.NodeCount() == 1U && scene.Nodes()[0].x == 4);
}

void TextureReplacementRequiresACompleteKindChange() {
    SceneStorage<2U, 2U> storage;
    graphics::GuestScene& scene = storage.scene;
    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 1U, 0U, 1U, 1U, 0U);
    keyframe.Add(Background(0U));
    keyframe.Add(Rect(1, kCommonMask | kKind));
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
                .reserved0 = 0U,
                .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                .property_mask = kContentMask | kKind,
            },
        .x = 2,
        .y = 1,
        .width = 2,
        .height = 2,
        .texture_handle = 7U,
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
    assert(scene.Nodes()[0].kind == graphics::GuestSceneNodeKind::kTexture && scene.Nodes()[0].texture_handle == 7U);
}

void SpriteBatchInstancesPatchIndependently() {
    SceneStorage<2U, 4U> storage;
    graphics::GuestScene& scene = storage.scene;

    const micropixel_graphics_scene_sprite_batch_record_t batch{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
                            .size = sizeof(micropixel_graphics_scene_sprite_batch_record_t)},
                 .node_id = 0U,
                 .reserved0 = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask = kBatchMask},
        .texture_handle = 7U,
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

void AdaptiveAtlasFarEdgeRoundingIsNormalized() {
    SceneStorage<1U, 1U> storage;
    graphics::GuestScene& scene = storage.scene;

    const micropixel_graphics_scene_sprite_batch_record_t batch{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
                            .size = sizeof(micropixel_graphics_scene_sprite_batch_record_t)},
                 .node_id = 0U,
                 .reserved0 = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask = kBatchMask},
        .texture_handle = 8U,
        .capacity = 1U,
        .opacity = 255U,
        .reserved0 = 0U,
    };
    const micropixel_graphics_scene_sprite_instance_t rounded_instance{
        .x = 0,
        .y = 0,
        .width = 4,
        .height = 2,
        .source_x = 0,
        .source_y = 22,
        .source_width = 128,
        .source_height = 22,
        .opacity = 255U,
        .flags = MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE,
        .reserved0 = 0U,
    };
    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 4U, 0U, 1U, 1U, 0U, 1U);
    keyframe.Add(Background(0U));
    keyframe.Add(batch);
    keyframe.AddInstances(0U, 0U, kInstanceMask, {rounded_instance});
    const auto& keyframe_bytes = keyframe.Finish();
    assert(scene.Apply(keyframe_bytes.data(), static_cast<uint32_t>(keyframe_bytes.size()), 8, 4, ResolveBitmap,
                       nullptr, ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Instances()[0].source_y == 22 && scene.Instances()[0].source_height == 21);

    auto invalid_instance = rounded_instance;
    invalid_instance.source_height = 23;
    Message invalid(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 5U, 0U, 1U, 1U, 0U, 1U);
    invalid.Add(Background(0U));
    invalid.Add(batch);
    invalid.AddInstances(0U, 0U, kInstanceMask, {invalid_instance});
    const auto& invalid_bytes = invalid.Finish();
    assert(scene.Apply(invalid_bytes.data(), static_cast<uint32_t>(invalid_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
}

void ContainerTreeIsValidatedAndPatchedAtomically() {
    SceneStorage<4U, 1U> storage;
    graphics::GuestScene& scene = storage.scene;

    auto child = Rect(0, kCommonMask | kKind);
    auto root_child = Rect(4, kCommonMask | kKind);
    root_child.node.node_id = 1U;
    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 7U, 0U, 1U, 2U, 2U, 0U);
    keyframe.Add(Background(0U));
    keyframe.Add(Container(1U, 0U, 0U, 1));
    keyframe.Add(Container(2U, 1U, 1U, 2));
    keyframe.Add(child);
    keyframe.Add(Link(0U, 2U, 2U));
    keyframe.Add(root_child);
    keyframe.Add(Link(1U, 0U, 3U));
    const auto& bytes = keyframe.Finish();
    assert(scene.Apply(bytes.data(), static_cast<uint32_t>(bytes.size()), 8, 4, ResolveBitmap, nullptr, ValidateFont,
                       nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.ContainerCount() == 2U && scene.Nodes()[0].parent_container_id == 2U);
    assert(scene.DrawNodeId(0U) == 0U && scene.DrawNodeId(1U) == 1U);

    Message patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 7U, 1U, 2U, 2U, 2U, 0U);
    patch.Add(Container(1U, 0U, 0U, 3, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION));
    const auto& patch_bytes = patch.Finish();
    assert(scene.Apply(patch_bytes.data(), static_cast<uint32_t>(patch_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);
    assert((scene.AncestorChanges(2U) & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION) != 0U);
    assert(scene.AncestorChanges(0U) == 0U);

    Message cycle(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 8U, 0U, 1U, 1U, 2U, 0U);
    cycle.Add(Background(0U));
    cycle.Add(Container(1U, 2U, 0U));
    cycle.Add(Container(2U, 1U, 1U));
    cycle.Add(child);
    cycle.Add(Link(0U, 2U, 2U));
    const auto& cycle_bytes = cycle.Finish();
    assert(scene.Apply(cycle_bytes.data(), static_cast<uint32_t>(cycle_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                       ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(scene.Generation() == 7U && scene.Revision() == 2U);
}

void RemovedProtocolsAreRejectedAtomically() {
    SceneStorage<2U, 1U> storage;
    auto& scene = storage.scene;
    Message message(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 42U, 0U, 1U, 1U, 0U, 0U);
    message.Add(Background(0U));
    message.Add(Rect(0));
    const auto original = message.Finish();
    const auto apply = [&](const std::vector<uint8_t>& bytes) {
        return scene.Apply(bytes.data(), static_cast<uint32_t>(bytes.size()), 8, 4, ResolveBitmap, nullptr,
                           ValidateFont, nullptr);
    };
    assert(apply(original) == MICROPIXEL_STATUS_OK);
    const auto reject = [&](const std::vector<uint8_t>& bytes) {
        assert(apply(bytes) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
        assert(scene.Generation() == 42U && scene.Revision() == 1U && scene.NodeCount() == 1U);
    };
    auto flagged = original;
    micropixel_graphics_scene_header_t flagged_header{};
    std::memcpy(&flagged_header, flagged.data(), sizeof(flagged_header));
    flagged_header.flags = 1U;
    std::memcpy(flagged.data(), &flagged_header, sizeof(flagged_header));
    reject(flagged);
    auto old_opcode = original;
    micropixel_graphics_scene_record_header_t record{.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES + 1U,
                                                     .size = 40U};
    std::memcpy(old_opcode.data() + sizeof(micropixel_graphics_scene_header_t), &record, sizeof(record));
    reject(old_opcode);
    const size_t node_offset =
        sizeof(micropixel_graphics_scene_header_t) + sizeof(micropixel_graphics_scene_background_record_t);
    for (bool old_parent : {false, true}) {
        auto bytes = original;
        micropixel_graphics_scene_node_header_t node{};
        std::memcpy(&node, bytes.data() + node_offset, sizeof(node));
        if (old_parent)
            node.reserved0 = 1U;
        else
            node.property_mask |= 1U << 5U;  // Above the last defined node property bit.
        std::memcpy(bytes.data() + node_offset, &node, sizeof(node));
        reject(bytes);
    }
    auto missing_link = original;
    missing_link.resize(missing_link.size() - sizeof(micropixel_graphics_scene_node_link_record_t));
    micropixel_graphics_scene_header_t header{};
    std::memcpy(&header, missing_link.data(), sizeof(header));
    --header.record_count;
    header.total_size = static_cast<uint32_t>(missing_link.size());
    std::memcpy(missing_link.data(), &header, sizeof(header));
    reject(missing_link);
}

void ContainerFlagsAreValidated() {
    constexpr uint32_t kFullContainerMask = kContainerMask | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS;
    constexpr uint16_t kCached = MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAG_CACHED_CONTENT;
    SceneStorage<2U, 1U> storage;
    graphics::GuestScene& scene = storage.scene;
    auto child = Rect(0, kCommonMask | kKind);
    const auto apply = [&](Message& message) {
        const auto& bytes = message.Finish();
        return scene.Apply(bytes.data(), static_cast<uint32_t>(bytes.size()), 8, 4, ResolveBitmap, nullptr,
                           ValidateFont, nullptr);
    };

    // A keyframe must carry the full mask and may set the known flag.
    Message short_mask(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 3U, 0U, 1U, 1U, 1U, 0U);
    short_mask.Add(Background(0U));
    short_mask.Add(Container(1U, 0U, 0U, 0, kContainerMask & ~MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS));
    short_mask.Add(child);
    short_mask.Add(Link(0U, 1U, 1U));
    assert(apply(short_mask) == MICROPIXEL_STATUS_INVALID_ARGUMENT);

    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 3U, 0U, 1U, 1U, 1U, 0U);
    keyframe.Add(Background(0U));
    auto cached = Container(1U, 0U, 0U, 0, kFullContainerMask);
    cached.flags = kCached;
    keyframe.Add(cached);
    keyframe.Add(child);
    keyframe.Add(Link(0U, 1U, 1U));
    assert(apply(keyframe) == MICROPIXEL_STATUS_OK);
    assert(scene.Containers()[1].cached_content);

    // Unknown flag bits are rejected.
    Message unknown(MICROPIXEL_GRAPHICS_SCENE_PATCH, 3U, 1U, 2U, 1U, 1U, 0U);
    auto bogus = Container(1U, 0U, 0U, 0, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS);
    bogus.flags = static_cast<uint16_t>(kCached | (1U << 7U));
    unknown.Add(bogus);
    assert(apply(unknown) == MICROPIXEL_STATUS_INVALID_ARGUMENT);

    // A translation-only patch must echo the retained flag value...
    Message mismatch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 3U, 1U, 2U, 1U, 1U, 0U);
    mismatch.Add(Container(1U, 0U, 0U, 3, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION));
    assert(apply(mismatch) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    Message translate(MICROPIXEL_GRAPHICS_SCENE_PATCH, 3U, 1U, 2U, 1U, 1U, 0U);
    auto moved = Container(1U, 0U, 0U, 3, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION);
    moved.flags = kCached;
    translate.Add(moved);
    assert(apply(translate) == MICROPIXEL_STATUS_OK);
    assert(scene.Containers()[1].cached_content && scene.Containers()[1].translate_x == 3);
    assert((scene.ContainerChanges(1U) & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS) == 0U);

    // ...and a FLAGS patch clears it.
    Message clear(MICROPIXEL_GRAPHICS_SCENE_PATCH, 3U, 2U, 3U, 1U, 1U, 0U);
    auto plain = Container(1U, 0U, 0U, 3, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS);
    clear.Add(plain);
    assert(apply(clear) == MICROPIXEL_STATUS_OK);
    assert(!scene.Containers()[1].cached_content);
    assert((scene.ContainerChanges(1U) & MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS) != 0U);
}

void RootViewportAcceptsOffscreenLocalGeometry() {
    SceneStorage<2U, 1U> storage;
    graphics::GuestScene& scene = storage.scene;

    auto prefetched = Rect(7, kCommonMask | kKind);
    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 9U, 0U, 1U, 1U, 2U, 0U);
    keyframe.Add(Background(0U));
    keyframe.Add(Container(1U, 0U, 0U));
    keyframe.Add(Container(2U, 1U, 1U, -1));
    keyframe.Add(prefetched);
    keyframe.Add(Link(0U, 2U, 2U));
    const auto& bytes = keyframe.Finish();
    assert(scene.Apply(bytes.data(), static_cast<uint32_t>(bytes.size()), 8, 4, ResolveBitmap, nullptr, ValidateFont,
                       nullptr) == MICROPIXEL_STATUS_OK);
    assert(scene.Nodes()[0].x == 7 && scene.Nodes()[0].width == 2);

    SceneStorage<2U, 1U> root_storage;
    graphics::GuestScene& root_scene = root_storage.scene;
    Message root_keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 10U, 0U, 1U, 1U, 0U);
    root_keyframe.Add(Background(0U));
    root_keyframe.Add(Rect(7, kCommonMask | kKind));
    const auto& root_bytes = root_keyframe.Finish();
    assert(root_scene.Apply(root_bytes.data(), static_cast<uint32_t>(root_bytes.size()), 8, 4, ResolveBitmap, nullptr,
                            ValidateFont, nullptr) == MICROPIXEL_STATUS_OK);

    SceneStorage<2U, 1U> invalid_storage;
    graphics::GuestScene& invalid_scene = invalid_storage.scene;
    auto empty = Rect(7, kCommonMask | kKind);
    empty.width = 0;
    Message invalid(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 11U, 0U, 1U, 1U, 0U);
    invalid.Add(Background(0U));
    invalid.Add(empty);
    const auto& invalid_bytes = invalid.Finish();
    assert(invalid_scene.Apply(invalid_bytes.data(), static_cast<uint32_t>(invalid_bytes.size()), 8, 4, ResolveBitmap,
                               nullptr, ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
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

void TextLivesInTheArenaAcrossPatches() {
    // 16-byte arena halves: enough to see appends, exhaustion and compaction.
    SceneStorage<2U, 1U, 1U, 16U> storage;
    graphics::GuestScene& scene = storage.scene;
    const auto apply = [&](Message& message) {
        const auto& bytes = message.Finish();
        return scene.Apply(bytes.data(), static_cast<uint32_t>(bytes.size()), 8, 4, ResolveBitmap, nullptr,
                           ValidateFont, nullptr);
    };
    const auto text_patch = [&](uint32_t revision, std::string_view text) {
        Message patch(MICROPIXEL_GRAPHICS_SCENE_PATCH, 3U, revision - 1U, revision, 1U, 0U);
        patch.AddText(0U, MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT, text);
        return apply(patch);
    };

    Message keyframe(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 3U, 0U, 1U, 1U, 0U);
    keyframe.Add(Background(0U));
    keyframe.AddText(0U, kContentMask | kKind, "hello");
    keyframe.Add(Link(0U, 0U, 0U));
    assert(apply(keyframe) == MICROPIXEL_STATUS_OK);
    assert(std::string_view(scene.Text(scene.Nodes()[0])) == "hello");
    assert(scene.TextUsed() == 6U && scene.TextLive() == 6U && scene.TextCapacity() == 16U);

    // Replaced text is appended; the old bytes stay until a compaction.
    const char* first = scene.Text(scene.Nodes()[0]);
    assert(text_patch(2U, "world!") == MICROPIXEL_STATUS_OK);
    assert(std::string_view(scene.Text(scene.Nodes()[0])) == "world!" && std::string_view(first) == "hello");
    assert(scene.TextUsed() == 13U && scene.TextLive() == 7U);
    assert(text_patch(3U, "x") == MICROPIXEL_STATUS_OK && scene.TextUsed() == 15U);

    // The owner must size the arena; without room the patch fails atomically.
    assert(text_patch(4U, "yy") == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    assert(scene.Revision() == 3U && std::string_view(scene.Text(scene.Nodes()[0])) == "x");

    // Compaction moves live text to the other half and keeps the old readable.
    const char* before = scene.Text(scene.Nodes()[0]);
    scene.CompactText();
    assert(scene.TextUsed() == 2U && std::string_view(before) == "x");
    assert(scene.Text(scene.Nodes()[0]) != before && std::string_view(scene.Text(scene.Nodes()[0])) == "x");
    assert(text_patch(4U, "yy") == MICROPIXEL_STATUS_OK && scene.TextUsed() == 5U);
    assert(std::string_view(scene.Text(scene.Nodes()[0])) == "yy");

    // A single run is still bounded by the Host text policy.
    SceneStorage<2U, 1U, 1U, 4096U> large;
    const std::string long_text(micropixel::device::graphics_limits::kMaxTextBytes + 1U, 'a');
    Message rejected(MICROPIXEL_GRAPHICS_SCENE_KEYFRAME, 4U, 0U, 1U, 1U, 0U);
    rejected.Add(Background(0U));
    rejected.AddText(0U, kContentMask | kKind, long_text);
    rejected.Add(Link(0U, 0U, 0U));
    const auto& rejected_bytes = rejected.Finish();
    assert(large.scene.Apply(rejected_bytes.data(), static_cast<uint32_t>(rejected_bytes.size()), 8, 4, ResolveBitmap,
                             nullptr, ValidateFont, nullptr) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
}

int main() {
    KeyframeAndPatchesAreAtomicAndRevisioned();
    TextLivesInTheArenaAcrossPatches();
    WideViewportTranslationIsAcceptedButRemainsCanvasBounded();
    SmallerKeyframeRemovesOldNodes();
    TextureReplacementRequiresACompleteKindChange();
    SpriteBatchInstancesPatchIndependently();
    AdaptiveAtlasFarEdgeRoundingIsNormalized();
    ContainerTreeIsValidatedAndPatchedAtomically();
    RemovedProtocolsAreRejectedAtomically();
    ContainerFlagsAreValidated();
    RootViewportAcceptsOffscreenLocalGeometry();
    SceneTransactionsSerializeNetPropertyChanges();
    return 0;
}
