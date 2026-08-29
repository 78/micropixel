#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "runtime/display_transform.hpp"
#include "runtime/panic.hpp"
#include "runtime/scene_delta.hpp"
#include "sdk/graphics.hpp"
#include "sdk/resources.hpp"

namespace micropixel {
namespace {

constexpr uint32_t kBaseMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                               MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER;
constexpr uint32_t kCommonMask = kBaseMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY;
constexpr uint32_t kLayerMask = MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP | MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION |
                                MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;

enum class SceneNodeKind : uint16_t {
    kShape = MICROPIXEL_GRAPHICS_SCENE_OP_RECT,
    kSprite = MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE,
    kLabel = MICROPIXEL_GRAPHICS_SCENE_OP_TEXT,
    kSpriteBatch = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
};

struct SceneNodeData final {
    SceneNodeKind kind{};
    uint32_t dirty{};
    uint8_t layer{};
    bool visible{true};
    bool centered{};
    Rect destination{};
    Color color{Color::Black()};
    uint8_t opacity{255U};
    uint32_t texture{};
    uint32_t texture_logical_width{};
    uint32_t texture_logical_height{};
    uint32_t texture_physical_width{};
    uint32_t texture_physical_height{};
    Rect source{};
    uint16_t font{};
    uint16_t text_length{};
    uint16_t batch_capacity{};
    uint16_t batch_instance_offset{};
    char text[MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES + 1U]{};
};

struct SceneLayerData final {
    uint32_t dirty{};
    Rect clip{};
    Point translation{};
    int16_t z_order{};
    uint8_t opacity{255U};
    bool visible{true};
};

struct SceneInstanceData final {
    uint32_t dirty{};
    SpriteInstance value{};
};

struct SceneNodeUndo final {
    uint16_t id{};
    SceneNodeData value{};
};

struct SceneInstanceUndo final {
    uint16_t id{};
    SceneInstanceData value{};
};

struct SceneLayerUndo final {
    uint8_t id{};
    SceneLayerData value{};
};

void Copy(void* destination, const void* source, uint32_t length) {
    auto* out = static_cast<uint8_t*>(destination);
    const auto* in = static_cast<const uint8_t*>(source);
    for (uint32_t index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

void Clear(void* destination, uint32_t length) {
    auto* out = static_cast<uint8_t*>(destination);
    for (uint32_t index = 0U; index < length; ++index) {
        out[index] = 0U;
    }
}

uint16_t TextLength(const char* text) {
    if (text == nullptr) {
        runtime::Panic("scene.text.null", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    uint16_t length = 0U;
    while (length <= MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES && text[length] != '\0') {
        ++length;
    }
    if (length == 0U || length > MICROPIXEL_GRAPHICS_MAX_TEXT_BYTES) {
        runtime::Panic("scene.text.length", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return length;
}

Error StatusError(int32_t status) {
    if (status == MICROPIXEL_STATUS_INVALID_ARGUMENT || status == MICROPIXEL_STATUS_INVALID_MEMORY) {
        return Error{ErrorCode::kInvalidArgument};
    }
    if (status == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED) {
        return Error{ErrorCode::kResourceExhausted};
    }
    if (status == MICROPIXEL_STATUS_UNSUPPORTED) {
        return Error{ErrorCode::kUnsupported};
    }
    return Error{ErrorCode::kInternal};
}

class SceneWriter final {
   public:
    template <typename Value>
    bool Add(const Value& value) {
        if (size_ > sizeof(scene_wire) || sizeof(Value) > sizeof(scene_wire) - size_) {
            return false;
        }
        Copy(scene_wire + size_, &value, sizeof(Value));
        size_ += sizeof(Value);
        ++records_;
        return true;
    }

    bool AddText(micropixel_graphics_scene_text_record_t value, const char* text) {
        const uint32_t size = (sizeof(value) + value.text_length + 3U) & ~3U;
        if (size_ > sizeof(scene_wire) || size > sizeof(scene_wire) - size_) {
            return false;
        }
        value.node.record.size = static_cast<uint16_t>(size);
        Clear(scene_wire + size_, size);
        Copy(scene_wire + size_, &value, sizeof(value));
        Copy(scene_wire + size_ + sizeof(value), text, value.text_length);
        size_ += size;
        ++records_;
        return true;
    }

    bool AddInstances(micropixel_graphics_scene_batch_instances_record_t value, const SceneInstanceData* instances,
                      const SceneNodeData& batch, const detail::DisplayTransform& display) {
        const uint32_t size = sizeof(value) + static_cast<uint32_t>(value.instance_count) *
                                                  sizeof(micropixel_graphics_scene_sprite_instance_t);
        if (size_ > sizeof(scene_wire) || size > sizeof(scene_wire) - size_ || size > UINT16_MAX) {
            return false;
        }
        value.record.size = static_cast<uint16_t>(size);
        Copy(scene_wire + size_, &value, sizeof(value));
        uint32_t output = size_ + sizeof(value);
        for (uint16_t index = 0U; index < value.instance_count; ++index) {
            const SpriteInstance& instance = instances[index].value;
            const detail::PhysicalRect destination =
                batch.texture == 0U
                    ? detail::MapSceneRect(display, instance.destination.x, instance.destination.y,
                                           instance.destination.width, instance.destination.height)
                    : detail::MapSceneSizedRect(display, instance.destination.x, instance.destination.y,
                                                instance.destination.width, instance.destination.height);
            const detail::PhysicalRect source =
                batch.texture == 0U ? detail::PhysicalRect{}
                                    : detail::MapSizedRect(instance.source.x, instance.source.y, instance.source.width,
                                                           instance.source.height, batch.texture_logical_width,
                                                           batch.texture_logical_height, batch.texture_physical_width,
                                                           batch.texture_physical_height);
            const micropixel_graphics_scene_sprite_instance_t wire{
                .x = destination.x,
                .y = destination.y,
                .width = destination.width,
                .height = destination.height,
                .source_x = source.x,
                .source_y = source.y,
                .source_width = source.width,
                .source_height = source.height,
                .rgb888 = instance.color.rgb888(),
                .opacity = instance.opacity,
                .flags = static_cast<uint8_t>(instance.visible ? MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE : 0U),
                .reserved0 = 0U,
            };
            Copy(scene_wire + output, &wire, sizeof(wire));
            output += sizeof(wire);
        }
        size_ += size;
        ++records_;
        return true;
    }

    uint32_t size_{sizeof(micropixel_graphics_scene_header_t)};
    uint16_t records_{};
    alignas(4) static uint8_t scene_wire[MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES];
};

alignas(4) uint8_t SceneWriter::scene_wire[MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES]{};
micropixel_service_info_t graphics_scene_service{};
bool graphics_scene_service_open{};

}  // namespace

class SceneState final {
   public:
    void Reset(const SceneDescriptor& descriptor) {
        for (uint16_t index = 0U; index < MICROPIXEL_GRAPHICS_MAX_SCENE_NODES; ++index) {
            nodes[index] = {};
            node_saved[index] = false;
        }
        for (uint8_t index = 0U; index <= MICROPIXEL_GRAPHICS_MAX_LAYERS; ++index) {
            layers[index] = {};
            layer_saved[index] = false;
        }
        for (uint16_t index = 0U; index < MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES; ++index) {
            instances[index] = {};
            instance_saved[index] = false;
        }
        node_count = 0U;
        layer_count = 0U;
        batch_instance_count = 0U;
        logical_width = descriptor.logical_width;
        logical_height = descriptor.logical_height;
        display = detail::CurrentDisplayTransform();
        background_color = descriptor.background;
        background_dirty = true;
        generation = 0U;
        revision = 0U;
        valid = false;
        update_active = false;
        undo_node_count = 0U;
        undo_instance_count = 0U;
        undo_layer_count = 0U;
        background_saved = false;
    }

    SceneNodeData& Node(uint16_t id) {
        if (id >= node_count) {
            runtime::Panic("scene.node.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        return nodes[id];
    }

    SceneLayerData& LayerData(uint8_t id) {
        if (id == 0U || id > layer_count) {
            runtime::Panic("scene.layer.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        return layers[id];
    }

    void BeginTransaction() {
        for (uint16_t index = 0U; index < undo_node_count; ++index) {
            node_saved[node_undo[index].id] = false;
        }
        for (uint16_t index = 0U; index < undo_instance_count; ++index) {
            instance_saved[instance_undo[index].id] = false;
        }
        for (uint8_t index = 0U; index < undo_layer_count; ++index) {
            layer_saved[layer_undo[index].id] = false;
        }
        undo_node_count = 0U;
        undo_instance_count = 0U;
        undo_layer_count = 0U;
        background_saved = false;
        update_active = true;
    }

    const SceneNodeData& RememberNode(uint16_t id) {
        if (!node_saved[id]) {
            node_saved[id] = true;
            node_undo[undo_node_count++] = {.id = id, .value = nodes[id]};
            node_undo_slot[id] = static_cast<uint16_t>(undo_node_count - 1U);
        }
        return node_undo[node_undo_slot[id]].value;
    }

    const SceneInstanceData& RememberInstance(uint16_t id) {
        if (!instance_saved[id]) {
            instance_saved[id] = true;
            instance_undo[undo_instance_count++] = {.id = id, .value = instances[id]};
            instance_undo_slot[id] = static_cast<uint16_t>(undo_instance_count - 1U);
        }
        return instance_undo[instance_undo_slot[id]].value;
    }

    const SceneLayerData& RememberLayer(uint8_t id) {
        if (!layer_saved[id]) {
            layer_saved[id] = true;
            layer_undo[undo_layer_count++] = {.id = id, .value = layers[id]};
            layer_undo_slot[id] = static_cast<uint8_t>(undo_layer_count - 1U);
        }
        return layer_undo[layer_undo_slot[id]].value;
    }

    void RememberBackground() {
        if (!background_saved) {
            background_saved = true;
            background_undo = background_color;
            background_dirty_undo = background_dirty;
        }
    }

    void RollbackTransaction() {
        for (uint16_t index = 0U; index < undo_node_count; ++index) {
            nodes[node_undo[index].id] = node_undo[index].value;
        }
        for (uint16_t index = 0U; index < undo_instance_count; ++index) {
            instances[instance_undo[index].id] = instance_undo[index].value;
        }
        for (uint8_t index = 0U; index < undo_layer_count; ++index) {
            layers[layer_undo[index].id] = layer_undo[index].value;
        }
        if (background_saved) {
            background_color = background_undo;
            background_dirty = background_dirty_undo;
        }
        update_active = false;
    }

    void CommitTransaction() { update_active = false; }

    SceneNodeData nodes[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    SceneLayerData layers[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    SceneInstanceData instances[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    SceneNodeUndo node_undo[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    SceneInstanceUndo instance_undo[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    SceneLayerUndo layer_undo[MICROPIXEL_GRAPHICS_MAX_LAYERS]{};
    uint16_t node_undo_slot[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    uint16_t instance_undo_slot[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    uint8_t layer_undo_slot[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    bool node_saved[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    bool instance_saved[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    bool layer_saved[MICROPIXEL_GRAPHICS_MAX_LAYERS + 1U]{};
    Color background_color{Color::Black()};
    Color background_undo{Color::Black()};
    uint16_t node_count{};
    uint16_t batch_instance_count{};
    uint8_t layer_count{};
    uint32_t logical_width{};
    uint32_t logical_height{};
    detail::DisplayTransform display{};
    uint32_t generation{};
    uint32_t revision{};
    bool background_dirty{};
    bool background_dirty_undo{};
    uint16_t undo_node_count{};
    uint16_t undo_instance_count{};
    uint8_t undo_layer_count{};
    bool background_saved{};
    bool valid{};
    bool update_active{};
};

namespace {

SceneState scene_storage;
bool scene_active{};

uint32_t FullMask(SceneNodeKind kind) {
    uint32_t mask =
        (kind == SceneNodeKind::kSpriteBatch ? kBaseMask : kCommonMask) | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
    if (kind != SceneNodeKind::kShape) {
        mask |= MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
    }
    return mask;
}

bool SameText(const SceneNodeData& left, const SceneNodeData& right) {
    if (left.text_length != right.text_length) {
        return false;
    }
    for (uint16_t index = 0U; index < left.text_length; ++index) {
        if (left.text[index] != right.text[index]) {
            return false;
        }
    }
    return true;
}

bool SameTexture(const SceneNodeData& left, const SceneNodeData& right) {
    return left.texture == right.texture && left.texture_logical_width == right.texture_logical_width &&
           left.texture_logical_height == right.texture_logical_height &&
           left.texture_physical_width == right.texture_physical_width &&
           left.texture_physical_height == right.texture_physical_height;
}

void UpdateNodeGeometryDirty(SceneNodeData& node, const SceneNodeData& original) {
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY,
                                     node.destination != original.destination || node.centered != original.centered);
}

void UpdateNodeAppearanceDirty(SceneNodeData& node, const SceneNodeData& original) {
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE,
                                     node.color != original.color || node.opacity != original.opacity);
}

void UpdateNodeContentDirty(SceneNodeData& node, const SceneNodeData& original) {
    bool changed = !SameTexture(node, original);
    if (node.kind == SceneNodeKind::kSprite) {
        changed = changed || node.source != original.source;
    } else if (node.kind == SceneNodeKind::kLabel) {
        changed = node.font != original.font || !SameText(node, original);
    }
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT, changed);
}

void ValidateHandle(SceneState* state) {
    if (state != &scene_storage || !scene_active) {
        runtime::Panic("scene.handle.stale", MICROPIXEL_STATUS_CLOSED);
    }
}

void ValidateUpdate(SceneState* state, SceneUpdate& update) {
    ValidateHandle(state);
    if (!update.active_for(state) || !state->update_active) {
        runtime::Panic("scene.update.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
}

int32_t OpenGraphicsSceneService() {
    if (!graphics_scene_service_open) {
        const int32_t status = micropixel_service_open(
            MICROPIXEL_SERVICE_GRAPHICS,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_GRAPHICS_INTERFACE_MAJOR, MICROPIXEL_GRAPHICS_INTERFACE_MINOR),
            &graphics_scene_service, sizeof(graphics_scene_service));
        if (status != MICROPIXEL_STATUS_OK) {
            return status;
        }
        graphics_scene_service_open = true;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t EncodeAndSubmit(SceneState& state, bool keyframe) {
    SceneWriter writer;
    if (keyframe || state.background_dirty) {
        if (!writer.Add(micropixel_graphics_scene_background_record_t{
                .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND,
                           .size = sizeof(micropixel_graphics_scene_background_record_t)},
                .property_mask = MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR,
                .rgb888 = state.background_color.rgb888(),
            })) {
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
    }
    const uint8_t layer_records = keyframe ? state.layer_count : state.undo_layer_count;
    for (uint8_t record_index = 0U; record_index < layer_records; ++record_index) {
        const uint8_t id = keyframe ? static_cast<uint8_t>(record_index + 1U) : state.layer_undo[record_index].id;
        const SceneLayerData& layer = state.layers[id];
        const uint32_t mask = keyframe ? kLayerMask : layer.dirty;
        if (mask == 0U) {
            continue;
        }
        const detail::PhysicalRect clip =
            detail::MapSceneRect(state.display, layer.clip.x, layer.clip.y, layer.clip.width, layer.clip.height);
        if (!writer.Add(micropixel_graphics_scene_layer_record_t{
                .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_LAYER,
                           .size = sizeof(micropixel_graphics_scene_layer_record_t)},
                .layer_id = id,
                .reserved0 = 0U,
                .property_mask = mask,
                .clip_x = clip.x,
                .clip_y = clip.y,
                .width = clip.width,
                .height = clip.height,
                .translate_x = detail::MapSceneVectorX(state.display, layer.translation.x),
                .translate_y = detail::MapSceneVectorY(state.display, layer.translation.y),
                .z_order = layer.z_order,
                .opacity = layer.opacity,
                .visible = static_cast<uint8_t>(layer.visible ? 1U : 0U),
            })) {
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
    }
    const uint16_t node_records = keyframe ? state.node_count : state.undo_node_count;
    for (uint16_t record_index = 0U; record_index < node_records; ++record_index) {
        const uint16_t id = keyframe ? record_index : state.node_undo[record_index].id;
        const SceneNodeData& node = state.nodes[id];
        const uint32_t mask = keyframe ? FullMask(node.kind) : node.dirty;
        if (mask == 0U) {
            continue;
        }
        micropixel_graphics_scene_node_header_t header{
            .record = {.opcode = static_cast<uint16_t>(node.kind), .size = 0U},
            .node_id = id,
            .layer_id = node.layer,
            .flags = static_cast<uint8_t>((node.visible ? MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE : 0U) |
                                          (node.centered ? MICROPIXEL_GRAPHICS_SCENE_TEXT_CENTERED : 0U)),
            .property_mask = mask,
        };
        bool added = false;
        const detail::PhysicalRect destination =
            node.kind == SceneNodeKind::kSprite
                ? detail::MapSceneSizedRect(state.display, node.destination.x, node.destination.y,
                                            node.destination.width, node.destination.height)
                : detail::MapSceneRect(state.display, node.destination.x, node.destination.y, node.destination.width,
                                       node.destination.height);
        if (node.kind == SceneNodeKind::kShape) {
            auto value = micropixel_graphics_scene_rect_record_t{
                .node = header,
                .x = destination.x,
                .y = destination.y,
                .width = destination.width,
                .height = destination.height,
                .rgb888 = node.color.rgb888(),
                .opacity = node.opacity,
                .reserved0 = {},
            };
            value.node.record.size = sizeof(value);
            added = writer.Add(value);
        } else if (node.kind == SceneNodeKind::kSprite) {
            const detail::PhysicalRect source = detail::MapSizedRect(
                node.source.x, node.source.y, node.source.width, node.source.height, node.texture_logical_width,
                node.texture_logical_height, node.texture_physical_width, node.texture_physical_height);
            auto value = micropixel_graphics_scene_texture_record_t{
                .node = header,
                .x = destination.x,
                .y = destination.y,
                .width = destination.width,
                .height = destination.height,
                .texture = node.texture,
                .source_x = source.x,
                .source_y = source.y,
                .source_width = source.width,
                .source_height = source.height,
                .opacity = node.opacity,
                .reserved0 = {},
            };
            value.node.record.size = sizeof(value);
            added = writer.Add(value);
        } else if (node.kind == SceneNodeKind::kSpriteBatch) {
            auto value = micropixel_graphics_scene_sprite_batch_record_t{
                .node = header,
                .texture = node.texture,
                .capacity = node.batch_capacity,
                .opacity = node.opacity,
                .reserved0 = 0U,
            };
            value.node.record.size = sizeof(value);
            added = writer.Add(value);
        } else {
            added = writer.AddText(
                micropixel_graphics_scene_text_record_t{
                    .node = header,
                    .x = destination.x,
                    .y = destination.y,
                    .rgb888 = node.color.rgb888(),
                    .font = detail::MapSystemFont(node.font, state.display),
                    .text_length = static_cast<uint16_t>(
                        (mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U ? node.text_length : 0U),
                },
                node.text);
        }
        if (!added) {
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
    }
    uint16_t changed_instances[MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES]{};
    uint16_t changed_instance_count = 0U;
    if (!keyframe) {
        for (uint16_t index = 0U; index < state.undo_instance_count; ++index) {
            const uint16_t id = state.instance_undo[index].id;
            if (state.instances[id].dirty == 0U) {
                continue;
            }
            uint16_t insertion = changed_instance_count;
            while (insertion > 0U && changed_instances[insertion - 1U] > id) {
                changed_instances[insertion] = changed_instances[insertion - 1U];
                --insertion;
            }
            changed_instances[insertion] = id;
            ++changed_instance_count;
        }
    }
    uint16_t changed_index = 0U;
    uint16_t batch_node_cursor = 0U;
    while (keyframe ? batch_node_cursor < state.node_count : changed_index < changed_instance_count) {
        while (batch_node_cursor < state.node_count &&
               state.nodes[batch_node_cursor].kind != SceneNodeKind::kSpriteBatch) {
            ++batch_node_cursor;
        }
        if (batch_node_cursor >= state.node_count) {
            break;
        }
        const SceneNodeData& node = state.nodes[batch_node_cursor];
        uint16_t scene_instance = keyframe ? node.batch_instance_offset : changed_instances[changed_index];
        const uint16_t batch_end = node.batch_instance_offset + node.batch_capacity;
        if (!keyframe && scene_instance >= batch_end) {
            ++batch_node_cursor;
            continue;
        }
        const uint16_t local_instance = scene_instance - node.batch_instance_offset;
        const uint32_t mask = keyframe ? kInstanceMask : state.instances[scene_instance].dirty;
        uint16_t count = 1U;
        if (keyframe) {
            while (local_instance + count < node.batch_capacity) {
                ++count;
            }
        } else {
            while (changed_index + count < changed_instance_count &&
                   changed_instances[changed_index + count] == scene_instance + count &&
                   changed_instances[changed_index + count] < batch_end &&
                   state.instances[scene_instance + count].dirty == mask) {
                ++count;
            }
        }
        if (!writer.AddInstances(
                micropixel_graphics_scene_batch_instances_record_t{
                    .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES, .size = 0U},
                    .batch_node_id = batch_node_cursor,
                    .first_instance = local_instance,
                    .instance_count = count,
                    .reserved0 = 0U,
                    .property_mask = mask,
                },
                state.instances + scene_instance, node, state.display)) {
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        if (keyframe) {
            ++batch_node_cursor;
        } else {
            changed_index += count;
        }
    }
    if (!keyframe && writer.records_ == 0U) {
        return MICROPIXEL_STATUS_OK;
    }
    const uint32_t next_generation =
        keyframe ? (state.generation == UINT32_MAX ? 1U : state.generation + 1U) : state.generation;
    const uint32_t next_revision = keyframe ? 1U : state.revision + 1U;
    const micropixel_graphics_scene_header_t header{
        .magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC,
        .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR,
        .kind = static_cast<uint16_t>(keyframe ? MICROPIXEL_GRAPHICS_SCENE_KEYFRAME : MICROPIXEL_GRAPHICS_SCENE_PATCH),
        .flags = 0U,
        .total_size = writer.size_,
        .generation = next_generation,
        .base_revision = keyframe ? 0U : state.revision,
        .revision = next_revision,
        .record_count = writer.records_,
        .node_count = state.node_count,
        .layer_count = state.layer_count,
        .batch_instance_count = state.batch_instance_count,
    };
    Copy(SceneWriter::scene_wire, &header, sizeof(header));
    int32_t status = OpenGraphicsSceneService();
    if (status == MICROPIXEL_STATUS_OK) {
        status = micropixel_service_submit(graphics_scene_service.handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                           SceneWriter::scene_wire, writer.size_);
    }
    if (status == MICROPIXEL_STATUS_OK) {
        state.generation = next_generation;
        state.revision = next_revision;
        state.valid = true;
        state.background_dirty = false;
        if (keyframe) {
            for (uint8_t id = 1U; id <= state.layer_count; ++id) {
                state.layers[id].dirty = 0U;
            }
            for (uint16_t id = 0U; id < state.node_count; ++id) {
                state.nodes[id].dirty = 0U;
            }
            for (uint16_t id = 0U; id < state.batch_instance_count; ++id) {
                state.instances[id].dirty = 0U;
            }
        } else {
            for (uint8_t index = 0U; index < state.undo_layer_count; ++index) {
                state.layers[state.layer_undo[index].id].dirty = 0U;
            }
            for (uint16_t index = 0U; index < state.undo_node_count; ++index) {
                state.nodes[state.node_undo[index].id].dirty = 0U;
            }
            for (uint16_t index = 0U; index < state.undo_instance_count; ++index) {
                state.instances[state.instance_undo[index].id].dirty = 0U;
            }
        }
    }
    return status;
}

}  // namespace

void NodeHandle::SetVisible(SceneUpdate& update, bool visible) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.visible == visible) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.visible = visible;
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY,
                                     node.visible != original.visible);
}

void NodeHandle::SetLayer(SceneUpdate& update, Layer layer) {
    ValidateUpdate(state_, update);
    if (layer.state_ != nullptr && layer.state_ != state_) {
        runtime::Panic("scene.layer.owner", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SceneNodeData& node = state_->Node(id_);
    if (node.layer == layer.id_) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.layer = layer.id_;
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_LAYER,
                                     node.layer != original.layer);
}

void Layer::SetClip(SceneUpdate& update, Rect clip) {
    ValidateUpdate(state_, update);
    SceneLayerData& layer = state_->LayerData(id_);
    if (layer.clip == clip) {
        return;
    }
    const SceneLayerData& original = state_->RememberLayer(id_);
    layer.clip = clip;
    detail::UpdateScenePropertyDirty(layer.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_LAYER_CLIP,
                                     layer.clip != original.clip);
}

void Layer::SetTranslation(SceneUpdate& update, Point translation) {
    ValidateUpdate(state_, update);
    SceneLayerData& layer = state_->LayerData(id_);
    if (layer.translation == translation) {
        return;
    }
    const SceneLayerData& original = state_->RememberLayer(id_);
    layer.translation = translation;
    detail::UpdateScenePropertyDirty(layer.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_LAYER_TRANSLATION,
                                     layer.translation != original.translation);
}

void Layer::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneLayerData& layer = state_->LayerData(id_);
    if (layer.opacity == opacity) {
        return;
    }
    const SceneLayerData& original = state_->RememberLayer(id_);
    layer.opacity = opacity;
    detail::UpdateScenePropertyDirty(layer.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE,
                                     layer.opacity != original.opacity || layer.visible != original.visible);
}

void Layer::SetVisible(SceneUpdate& update, bool visible) {
    ValidateUpdate(state_, update);
    SceneLayerData& layer = state_->LayerData(id_);
    if (layer.visible == visible) {
        return;
    }
    const SceneLayerData& original = state_->RememberLayer(id_);
    layer.visible = visible;
    detail::UpdateScenePropertyDirty(layer.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_LAYER_APPEARANCE,
                                     layer.opacity != original.opacity || layer.visible != original.visible);
}

void Layer::SetZOrder(SceneUpdate& update, int16_t z_order) {
    ValidateUpdate(state_, update);
    SceneLayerData& layer = state_->LayerData(id_);
    if (layer.z_order == z_order) {
        return;
    }
    const SceneLayerData& original = state_->RememberLayer(id_);
    layer.z_order = z_order;
    detail::UpdateScenePropertyDirty(layer.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_LAYER_Z_ORDER,
                                     layer.z_order != original.z_order);
}

void ShapeNode::SetRect(SceneUpdate& update, Rect rect) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.destination == rect) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = rect;
    UpdateNodeGeometryDirty(node, original);
}

void ShapeNode::SetColor(SceneUpdate& update, Color color) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void ShapeNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SpriteNode::SetDestination(SceneUpdate& update, Rect destination) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.destination == destination) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = destination;
    UpdateNodeGeometryDirty(node, original);
}

void SpriteNode::SetSource(SceneUpdate& update, Rect source) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.source == source) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.source = source;
    UpdateNodeContentDirty(node, original);
}

void SpriteNode::SetTexture(SceneUpdate& update, const Texture& texture) {
    ValidateUpdate(state_, update);
    if (!texture.valid()) {
        runtime::Panic("scene.sprite.texture", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SceneNodeData& node = state_->Node(id_);
    if (node.texture == texture.handle_) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.texture = texture.handle_;
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    UpdateNodeContentDirty(node, original);
}

void SpriteNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SurfaceNode::SetDestination(SceneUpdate& update, Rect destination) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.destination == destination) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = destination;
    UpdateNodeGeometryDirty(node, original);
}

void SurfaceNode::SetSource(SceneUpdate& update, Rect source) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.source == source) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.source = source;
    UpdateNodeContentDirty(node, original);
}

void SurfaceNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void LabelNode::SetPosition(SceneUpdate& update, Point position) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.destination.x == position.x && node.destination.y == position.y) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination.x = position.x;
    node.destination.y = position.y;
    UpdateNodeGeometryDirty(node, original);
}

void LabelNode::SetText(SceneUpdate& update, const char* text) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    const uint16_t length = TextLength(text);
    bool changed = length != node.text_length;
    for (uint16_t index = 0U; !changed && index < length; ++index) {
        changed = node.text[index] != text[index];
    }
    if (!changed) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    Copy(node.text, text, length);
    node.text[length] = '\0';
    node.text_length = length;
    UpdateNodeContentDirty(node, original);
}

void LabelNode::SetColor(SceneUpdate& update, Color color) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void LabelNode::SetFont(SceneUpdate& update, SystemFont font) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    const uint16_t handle = static_cast<uint16_t>(font);
    if (handle == 0U) {
        runtime::Panic("scene.label.font", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (node.font == handle) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.font = handle;
    UpdateNodeContentDirty(node, original);
}

void LabelNode::SetCentered(SceneUpdate& update, bool centered) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.centered == centered) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.centered = centered;
    UpdateNodeGeometryDirty(node, original);
}

void SpriteBatch::SetTexture(SceneUpdate& update, const Texture& texture) {
    ValidateUpdate(state_, update);
    if (!texture.valid()) {
        runtime::Panic("scene.batch.texture", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SceneNodeData& node = state_->Node(id_);
    if (node.texture == texture.handle_) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.texture = texture.handle_;
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    UpdateNodeContentDirty(node, original);
}

void SpriteBatch::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SpriteBatch::SetInstance(SceneUpdate& update, uint16_t instance_id, const SpriteInstance& instance) {
    ValidateUpdate(state_, update);
    const SceneNodeData& batch = state_->Node(id_);
    if (batch.kind != SceneNodeKind::kSpriteBatch || instance_id >= batch.batch_capacity) {
        runtime::Panic("scene.batch.instance", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint16_t scene_instance_id = batch.batch_instance_offset + instance_id;
    SceneInstanceData& target = state_->instances[batch.batch_instance_offset + instance_id];
    if (target.value.destination == instance.destination && target.value.source == instance.source &&
        target.value.color == instance.color && target.value.opacity == instance.opacity &&
        target.value.visible == instance.visible) {
        return;
    }
    const SceneInstanceData& original = state_->RememberInstance(scene_instance_id);
    if (target.value.destination != instance.destination) {
        target.value.destination = instance.destination;
        detail::UpdateScenePropertyDirty(target.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY,
                                         target.value.destination != original.value.destination);
    }
    if (target.value.source != instance.source) {
        target.value.source = instance.source;
        detail::UpdateScenePropertyDirty(target.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT,
                                         target.value.source != original.value.source);
    }
    if (target.value.color != instance.color || target.value.opacity != instance.opacity) {
        target.value.color = instance.color;
        target.value.opacity = instance.opacity;
        detail::UpdateScenePropertyDirty(
            target.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE,
            target.value.color != original.value.color || target.value.opacity != original.value.opacity);
    }
    if (target.value.visible != instance.visible) {
        target.value.visible = instance.visible;
        detail::UpdateScenePropertyDirty(target.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY,
                                         target.value.visible != original.value.visible);
    }
}

void SpriteBatch::SetInstanceVisible(SceneUpdate& update, uint16_t instance_id, bool visible) {
    ValidateUpdate(state_, update);
    const SceneNodeData& batch = state_->Node(id_);
    if (batch.kind != SceneNodeKind::kSpriteBatch || instance_id >= batch.batch_capacity) {
        runtime::Panic("scene.batch.instance", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    const uint16_t scene_instance_id = batch.batch_instance_offset + instance_id;
    SceneInstanceData& target = state_->instances[batch.batch_instance_offset + instance_id];
    if (target.value.visible == visible) {
        return;
    }
    const SceneInstanceData& original = state_->RememberInstance(scene_instance_id);
    target.value.visible = visible;
    detail::UpdateScenePropertyDirty(target.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY,
                                     target.value.visible != original.value.visible);
}

Scene::Scene(CapabilityToken, const SceneDescriptor& descriptor) : state_(&scene_storage) {
    if (scene_active) {
        runtime::Panic("scene.concurrent", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    scene_active = true;
    const detail::DisplayTransform& display = detail::CurrentDisplayTransform();
    if (descriptor.logical_width == 0U || descriptor.logical_height == 0U ||
        descriptor.logical_width != display.logical_width || descriptor.logical_height != display.logical_height) {
        runtime::Panic("scene.size", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    state_->Reset(descriptor);
}

Scene::Scene(Scene&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Scene::~Scene() {
    if (state_ != nullptr) {
        scene_active = false;
    }
}

SceneUpdate Scene::BeginUpdate() {
    ValidateHandle(state_);
    if (state_->update_active) {
        runtime::Panic("scene.update.concurrent", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    state_->BeginTransaction();
    return SceneUpdate{state_};
}

void Scene::SetBackground(SceneUpdate& update, Color color) {
    ValidateUpdate(state_, update);
    if (state_->background_color == color) {
        return;
    }
    state_->RememberBackground();
    state_->background_color = color;
    state_->background_dirty = state_->background_color != state_->background_undo || state_->background_dirty_undo;
}

Layer Scene::CreateLayer(const LayerProperties& properties) {
    ValidateHandle(state_);
    if (state_->layer_count >= MICROPIXEL_GRAPHICS_MAX_LAYERS) {
        runtime::Panic("scene.layers.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint8_t id = ++state_->layer_count;
    state_->layers[id] = {.dirty = kLayerMask,
                          .clip = properties.clip,
                          .translation = properties.translation,
                          .z_order = properties.z_order,
                          .opacity = properties.opacity,
                          .visible = properties.visible};
    state_->valid = false;
    return Layer{state_, id};
}

ShapeNode Scene::CreateShape(Rect rect, Color color, Layer layer, uint8_t opacity) {
    ValidateHandle(state_);
    if (static_cast<uint32_t>(state_->node_count) + state_->batch_instance_count >=
        MICROPIXEL_GRAPHICS_MAX_SCENE_NODES) {
        runtime::Panic("scene.nodes.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->node_count++;
    state_->nodes[id] = {.kind = SceneNodeKind::kShape,
                         .dirty = FullMask(SceneNodeKind::kShape),
                         .layer = layer.id_,
                         .visible = true,
                         .centered = false,
                         .destination = rect,
                         .color = color,
                         .opacity = opacity};
    state_->valid = false;
    return ShapeNode{state_, id};
}

SpriteNode Scene::CreateSprite(const Texture& texture, Rect destination, Rect source, Layer layer, uint8_t opacity) {
    ValidateHandle(state_);
    if (!texture.valid() || static_cast<uint32_t>(state_->node_count) + state_->batch_instance_count >=
                                MICROPIXEL_GRAPHICS_MAX_SCENE_NODES) {
        runtime::Panic("scene.sprite.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->node_count++;
    state_->nodes[id] = {.kind = SceneNodeKind::kSprite,
                         .dirty = FullMask(SceneNodeKind::kSprite),
                         .layer = layer.id_,
                         .visible = true,
                         .centered = false,
                         .destination = destination,
                         .color = Color::Black(),
                         .opacity = opacity,
                         .texture = texture.handle_,
                         .texture_logical_width = texture.width_,
                         .texture_logical_height = texture.height_,
                         .texture_physical_width = texture.physical_width_,
                         .texture_physical_height = texture.physical_height_,
                         .source = source};
    state_->valid = false;
    return SpriteNode{state_, id};
}

SurfaceNode Scene::CreateSurfaceNode(const StreamingTexture& surface, Rect destination, Rect source, Layer layer,
                                     uint8_t opacity) {
    ValidateHandle(state_);
    if (!surface.valid() || static_cast<uint32_t>(state_->node_count) + state_->batch_instance_count >=
                                MICROPIXEL_GRAPHICS_MAX_SCENE_NODES) {
        runtime::Panic("scene.surface.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->node_count++;
    state_->nodes[id] = {.kind = SceneNodeKind::kSprite,
                         .dirty = FullMask(SceneNodeKind::kSprite),
                         .layer = layer.id_,
                         .visible = true,
                         .centered = false,
                         .destination = destination,
                         .color = Color::Black(),
                         .opacity = opacity,
                         .texture = surface.texture_.handle_,
                         .texture_logical_width = surface.texture_.width_,
                         .texture_logical_height = surface.texture_.height_,
                         .texture_physical_width = surface.texture_.physical_width_,
                         .texture_physical_height = surface.texture_.physical_height_,
                         .source = source};
    state_->valid = false;
    return SurfaceNode{state_, id};
}

SpriteBatch Scene::CreateSpriteBatch(const Texture& texture, uint16_t capacity, Layer layer, uint8_t opacity) {
    if (!texture.valid()) {
        runtime::Panic("scene.batch.texture", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SpriteBatch batch = CreateSpriteBatchInternal(texture.handle_, capacity, layer, opacity);
    SceneNodeData& node = state_->Node(batch.id_);
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    return batch;
}

SpriteBatch Scene::CreateSpriteBatch(uint16_t capacity, Layer layer, uint8_t opacity) {
    return CreateSpriteBatchInternal(0U, capacity, layer, opacity);
}

SpriteBatch Scene::CreateSpriteBatchInternal(uint32_t texture, uint16_t capacity, Layer layer, uint8_t opacity) {
    ValidateHandle(state_);
    if (capacity == 0U || capacity > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES ||
        static_cast<uint32_t>(state_->node_count) + state_->batch_instance_count + capacity >
            MICROPIXEL_GRAPHICS_MAX_SCENE_NODES ||
        static_cast<uint32_t>(state_->batch_instance_count) + capacity > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES) {
        runtime::Panic("scene.batch.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    uint8_t batch_count = 0U;
    for (uint16_t index = 0U; index < state_->node_count; ++index) {
        batch_count += state_->nodes[index].kind == SceneNodeKind::kSpriteBatch ? 1U : 0U;
    }
    if (batch_count >= MICROPIXEL_GRAPHICS_MAX_SPRITE_BATCHES) {
        runtime::Panic("scene.batches.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->node_count++;
    const uint16_t instance_offset = state_->batch_instance_count;
    state_->batch_instance_count += capacity;
    state_->nodes[id] = {.kind = SceneNodeKind::kSpriteBatch,
                         .dirty = FullMask(SceneNodeKind::kSpriteBatch),
                         .layer = layer.id_,
                         .visible = true,
                         .centered = false,
                         .destination = {},
                         .color = Color::Black(),
                         .opacity = opacity,
                         .texture = texture,
                         .source = {},
                         .font = 0U,
                         .text_length = 0U,
                         .batch_capacity = capacity,
                         .batch_instance_offset = instance_offset};
    for (uint16_t instance = 0U; instance < capacity; ++instance) {
        state_->instances[instance_offset + instance] = {.dirty = kInstanceMask, .value = {}};
    }
    state_->valid = false;
    return SpriteBatch{state_, id, capacity};
}

LabelNode Scene::CreateLabel(Point position, const char* text, Color color, SystemFont font, Layer layer,
                             bool centered) {
    ValidateHandle(state_);
    if (static_cast<uint32_t>(state_->node_count) + state_->batch_instance_count >=
        MICROPIXEL_GRAPHICS_MAX_SCENE_NODES) {
        runtime::Panic("scene.label.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->node_count++;
    SceneNodeData& node = state_->nodes[id];
    node = {.kind = SceneNodeKind::kLabel,
            .dirty = FullMask(SceneNodeKind::kLabel),
            .layer = layer.id_,
            .visible = true,
            .centered = centered,
            .destination = {.x = position.x, .y = position.y},
            .color = color,
            .opacity = 255U,
            .font = static_cast<uint16_t>(font)};
    const uint16_t length = TextLength(text);
    Copy(node.text, text, length);
    node.text[length] = '\0';
    node.text_length = length;
    state_->valid = false;
    return LabelNode{state_, id};
}

SceneUpdate::SceneUpdate(SceneUpdate&& other) noexcept : state_(other.state_), active_(other.active_) {
    other.state_ = nullptr;
    other.active_ = false;
}

SceneUpdate::~SceneUpdate() {
    if (active_ && state_ != nullptr) {
        state_->RollbackTransaction();
    }
}

Result<void> SceneUpdate::Present() {
    if (!active_ || state_ == nullptr || !state_->update_active) {
        runtime::Panic("scene.update.present", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    int32_t status = EncodeAndSubmit(*state_, !state_->valid || state_->revision == UINT32_MAX);
    if (status == MICROPIXEL_STATUS_STALE_STATE) {
        state_->valid = false;
        status = EncodeAndSubmit(*state_, true);
    }
    if (status == MICROPIXEL_STATUS_OK) {
        state_->CommitTransaction();
    } else {
        state_->RollbackTransaction();
    }
    active_ = false;
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(StatusError(status))};
}

uint16_t Scene::node_count() const {
    ValidateHandle(state_);
    return state_->node_count;
}

Scene Renderer::CreateScene(const SceneDescriptor& descriptor) const {
    return Scene{Scene::CapabilityToken{}, descriptor};
}

}  // namespace micropixel
