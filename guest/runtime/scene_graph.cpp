#include <stdint.h>

#include <vector>

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
constexpr uint32_t kContainerMask =
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;
constexpr uint16_t kNoUndoSlot = UINT16_MAX;

enum class SceneNodeKind : uint16_t {
    kShape = MICROPIXEL_GRAPHICS_SCENE_OP_RECT,
    kRoundedRect = MICROPIXEL_GRAPHICS_SCENE_OP_ROUNDED_RECT,
    kSprite = MICROPIXEL_GRAPHICS_SCENE_OP_TEXTURE,
    kLabel = MICROPIXEL_GRAPHICS_SCENE_OP_TEXT,
    kSpriteBatch = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
};

struct SceneNodeData final {
    SceneNodeKind kind{};
    uint32_t dirty{};
    uint16_t parent_container_id{};
    bool visible{true};
    bool centered{};
    Rect destination{};
    Color color{Color::Black()};
    Color stroke_color{Color::Black()};
    uint32_t radius{};
    uint32_t stroke_width{};
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
    uint32_t order{};
    uint32_t generation{1U};
    uint16_t wire_id{UINT16_MAX};
    bool occupied{};
};

struct SceneContainerData final {
    uint32_t dirty{};
    Rect clip{};
    Point translation{};
    int16_t z_order{};
    uint8_t opacity{255U};
    bool visible{true};
    uint32_t order{};
    uint32_t generation{1U};
    uint16_t wire_id{UINT16_MAX};
    uint16_t parent_id{};
    bool occupied{};
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

struct SceneContainerUndo final {
    uint16_t id{};
    SceneContainerData value{};
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
                batch.texture == 0U
                    ? detail::PhysicalRect{}
                    : detail::MapTextureRect(instance.source.x, instance.source.y, instance.source.width,
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
        for (uint16_t index = 0U; index < nodes.size(); ++index) {
            const uint32_t next_generation = nodes[index].generation == 0U || nodes[index].generation == UINT32_MAX
                                                 ? 1U
                                                 : nodes[index].generation + 1U;
            nodes[index] = {};
            nodes[index].generation = next_generation;
        }
        if (containers.empty()) {
            containers.emplace_back();
        }
        for (uint16_t index = 0U; index < containers.size(); ++index) {
            const uint32_t next_generation =
                containers[index].generation == 0U || containers[index].generation == UINT32_MAX
                    ? 1U
                    : containers[index].generation + 1U;
            containers[index] = {};
            containers[index].generation = next_generation;
        }
        instances.clear();
        node_undo.clear();
        instance_undo.clear();
        container_undo.clear();
        node_undo_slot.assign(nodes.size(), kNoUndoSlot);
        instance_undo_slot.clear();
        container_undo_slot.assign(containers.size(), kNoUndoSlot);
        node_count = 0U;
        container_count = 0U;
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
        undo_container_count = 0U;
        background_saved = false;
        next_order = 0U;
        next_order_undo = 0U;
        valid_undo = false;
    }

    [[nodiscard]] bool NodeValid(uint16_t id, uint32_t handle_generation) const {
        return id < nodes.size() && nodes[id].occupied && handle_generation != 0U &&
               nodes[id].generation == handle_generation;
    }

    SceneNodeData& Node(uint16_t id, uint32_t handle_generation) {
        if (!NodeValid(id, handle_generation)) {
            runtime::Panic("scene.node.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        return nodes[id];
    }

    [[nodiscard]] bool ContainerValid(uint16_t id, uint32_t handle_generation) const {
        return id > 0U && id < containers.size() && containers[id].occupied && handle_generation != 0U &&
               containers[id].generation == handle_generation;
    }

    uint16_t ParentId(const Container& parent) const {
        if (parent.state_ == nullptr) {
            return 0U;
        }
        if (parent.state_ != this ||
            (parent.id_ == 0U ? parent.generation_ != 0U : !ContainerValid(parent.id_, parent.generation_))) {
            runtime::Panic("scene.container.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        return parent.id_;
    }

    uint16_t AllocateNode(const Container& parent) {
        if (static_cast<uint32_t>(node_count) + batch_instance_count >= MICROPIXEL_GRAPHICS_MAX_SCENE_NODES) {
            runtime::Panic("scene.nodes.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
        const uint16_t parent_id = ParentId(parent);
        uint16_t id = 0U;
        while (id < nodes.size() && nodes[id].occupied) {
            ++id;
        }
        if (id == MICROPIXEL_GRAPHICS_MAX_SCENE_NODES || next_order == UINT32_MAX) {
            runtime::Panic("scene.nodes.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
        if (id == nodes.size()) {
            nodes.emplace_back();
            node_undo_slot.push_back(kNoUndoSlot);
        }
        if (update_active) {
            RememberNode(id);
        }
        const uint32_t generation = nodes[id].generation == 0U ? 1U : nodes[id].generation;
        nodes[id] = {};
        nodes[id].generation = generation;
        nodes[id].wire_id = UINT16_MAX;
        nodes[id].parent_container_id = parent_id;
        nodes[id].order = ++next_order;
        nodes[id].occupied = true;
        ++node_count;
        valid = false;
        return id;
    }

    uint16_t AllocateContainer(const Container& parent, const ContainerProperties& properties) {
        if (container_count >= MICROPIXEL_GRAPHICS_MAX_CONTAINERS || next_order == UINT32_MAX) {
            runtime::Panic("scene.containers.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
        const uint16_t parent_id = ParentId(parent);
        uint16_t id = 1U;
        while (id < containers.size() && containers[id].occupied) {
            ++id;
        }
        if (id > MICROPIXEL_GRAPHICS_MAX_CONTAINERS) {
            runtime::Panic("scene.containers.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        }
        if (id == containers.size()) {
            containers.emplace_back();
            container_undo_slot.push_back(kNoUndoSlot);
        }
        if (update_active) {
            RememberContainer(id);
        }
        const uint32_t generation = containers[id].generation == 0U ? 1U : containers[id].generation;
        containers[id] = {.dirty = kContainerMask,
                          .clip = properties.clip,
                          .translation = properties.translation,
                          .z_order = properties.z_order,
                          .opacity = properties.opacity,
                          .visible = properties.visible,
                          .order = ++next_order,
                          .generation = generation,
                          .wire_id = UINT16_MAX,
                          .parent_id = parent_id,
                          .occupied = true};
        ++container_count;
        valid = false;
        return id;
    }

    SceneContainerData& Container(uint16_t id, uint32_t handle_generation) {
        if (!ContainerValid(id, handle_generation)) {
            runtime::Panic("scene.container.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        return containers[id];
    }

    void BeginTransaction() {
        for (uint16_t index = 0U; index < undo_node_count; ++index) {
            node_undo_slot[node_undo[index].id] = kNoUndoSlot;
        }
        for (uint16_t index = 0U; index < undo_instance_count; ++index) {
            instance_undo_slot[instance_undo[index].id] = kNoUndoSlot;
        }
        for (uint16_t index = 0U; index < undo_container_count; ++index) {
            container_undo_slot[container_undo[index].id] = kNoUndoSlot;
        }
        node_undo.clear();
        instance_undo.clear();
        container_undo.clear();
        undo_node_count = 0U;
        undo_instance_count = 0U;
        undo_container_count = 0U;
        background_saved = false;
        node_count_undo = node_count;
        batch_instance_count_undo = batch_instance_count;
        container_count_undo = container_count;
        next_order_undo = next_order;
        valid_undo = valid;
        update_active = true;
    }

    const SceneNodeData& RememberNode(uint16_t id) {
        if (node_undo_slot[id] == kNoUndoSlot) {
            node_undo_slot[id] = undo_node_count++;
            node_undo.push_back({.id = id, .value = nodes[id]});
        }
        return node_undo[node_undo_slot[id]].value;
    }

    const SceneInstanceData& RememberInstance(uint16_t id) {
        if (id >= instance_undo_slot.size()) {
            instance_undo_slot.resize(static_cast<size_t>(id) + 1U, kNoUndoSlot);
        }
        if (instance_undo_slot[id] == kNoUndoSlot) {
            instance_undo_slot[id] = undo_instance_count++;
            instance_undo.push_back({.id = id, .value = instances[id]});
        }
        return instance_undo[instance_undo_slot[id]].value;
    }

    const SceneContainerData& RememberContainer(uint16_t id) {
        if (container_undo_slot[id] == kNoUndoSlot) {
            container_undo_slot[id] = undo_container_count++;
            container_undo.push_back({.id = id, .value = containers[id]});
        }
        return container_undo[container_undo_slot[id]].value;
    }

    static uint32_t NextHandleGeneration(uint32_t generation) {
        return generation == UINT32_MAX ? 1U : generation + 1U;
    }

    void DestroyNode(uint16_t id, uint32_t handle_generation) {
        if (!NodeValid(id, handle_generation)) {
            return;
        }
        SceneNodeData& removed = nodes[id];
        RememberNode(id);
        if (removed.kind == SceneNodeKind::kSpriteBatch) {
            const uint16_t first = removed.batch_instance_offset;
            const uint16_t count = removed.batch_capacity;
            const uint16_t old_count = batch_instance_count;
            for (uint16_t instance = first; instance < old_count; ++instance) {
                RememberInstance(instance);
            }
            for (uint16_t instance = first; static_cast<uint32_t>(instance) + count < old_count; ++instance) {
                instances[instance] = instances[instance + count];
            }
            for (uint16_t instance = static_cast<uint16_t>(old_count - count); instance < old_count; ++instance) {
                instances[instance] = {};
            }
            for (uint16_t node_id = 0U; node_id < nodes.size(); ++node_id) {
                SceneNodeData& batch = nodes[node_id];
                if (batch.occupied && batch.kind == SceneNodeKind::kSpriteBatch &&
                    batch.batch_instance_offset > first) {
                    RememberNode(node_id);
                    batch.batch_instance_offset = static_cast<uint16_t>(batch.batch_instance_offset - count);
                }
            }
            batch_instance_count = static_cast<uint16_t>(batch_instance_count - count);
        }
        removed.occupied = false;
        removed.generation = NextHandleGeneration(removed.generation);
        removed.wire_id = UINT16_MAX;
        --node_count;
        valid = false;
    }

    void DestroyContainer(uint16_t id, uint32_t handle_generation) {
        if (!ContainerValid(id, handle_generation)) {
            return;
        }
        bool subtree[MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U]{};
        subtree[id] = true;
        for (uint16_t pass = 0U; pass < container_count; ++pass) {
            bool changed = false;
            for (uint16_t container_id = 1U; container_id < containers.size(); ++container_id) {
                if (containers[container_id].occupied && !subtree[container_id] &&
                    subtree[containers[container_id].parent_id]) {
                    subtree[container_id] = true;
                    changed = true;
                }
            }
            if (!changed) {
                break;
            }
        }
        for (uint16_t node_id = 0U; node_id < nodes.size(); ++node_id) {
            if (nodes[node_id].occupied && subtree[nodes[node_id].parent_container_id]) {
                DestroyNode(node_id, nodes[node_id].generation);
            }
        }
        for (uint16_t container_id = 1U; container_id < containers.size(); ++container_id) {
            if (!subtree[container_id]) {
                continue;
            }
            RememberContainer(container_id);
            containers[container_id].occupied = false;
            containers[container_id].generation = NextHandleGeneration(containers[container_id].generation);
            containers[container_id].wire_id = UINT16_MAX;
            --container_count;
        }
        valid = false;
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
            const uint16_t id = node_undo[index].id;
            const uint32_t transaction_generation = nodes[id].generation;
            nodes[id] = node_undo[index].value;
            if (!nodes[id].occupied) {
                nodes[id].generation = NextHandleGeneration(transaction_generation);
            }
        }
        for (uint16_t index = 0U; index < undo_instance_count; ++index) {
            instances[instance_undo[index].id] = instance_undo[index].value;
        }
        for (uint16_t index = 0U; index < undo_container_count; ++index) {
            const uint16_t id = container_undo[index].id;
            const uint32_t transaction_generation = containers[id].generation;
            containers[id] = container_undo[index].value;
            if (!containers[id].occupied) {
                containers[id].generation = NextHandleGeneration(transaction_generation);
            }
        }
        node_count = node_count_undo;
        batch_instance_count = batch_instance_count_undo;
        container_count = container_count_undo;
        next_order = next_order_undo;
        valid = valid_undo;
        if (background_saved) {
            background_color = background_undo;
            background_dirty = background_dirty_undo;
        }
        update_active = false;
    }

    void CommitTransaction() { update_active = false; }

    std::vector<SceneNodeData> nodes{};
    std::vector<SceneContainerData> containers{};
    std::vector<SceneInstanceData> instances{};
    std::vector<SceneNodeUndo> node_undo{};
    std::vector<SceneInstanceUndo> instance_undo{};
    std::vector<SceneContainerUndo> container_undo{};
    std::vector<uint16_t> node_undo_slot{};
    std::vector<uint16_t> instance_undo_slot{};
    std::vector<uint16_t> container_undo_slot{};
    Color background_color{Color::Black()};
    Color background_undo{Color::Black()};
    uint16_t node_count{};
    uint16_t batch_instance_count{};
    uint16_t container_count{};
    uint32_t logical_width{};
    uint32_t logical_height{};
    detail::DisplayTransform display{};
    uint32_t generation{};
    uint32_t revision{};
    uint32_t next_order{};
    bool background_dirty{};
    bool background_dirty_undo{};
    uint16_t undo_node_count{};
    uint16_t undo_instance_count{};
    uint16_t undo_container_count{};
    uint16_t node_count_undo{};
    uint16_t batch_instance_count_undo{};
    uint16_t container_count_undo{};
    uint32_t next_order_undo{};
    bool background_saved{};
    bool valid_undo{};
    bool valid{};
    bool update_active{};
};

namespace {

SceneState scene_storage __attribute__((no_destroy));
bool scene_active{};

uint32_t FullMask(SceneNodeKind kind) {
    uint32_t mask =
        (kind == SceneNodeKind::kSpriteBatch ? kBaseMask : kCommonMask) | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
    if (kind != SceneNodeKind::kShape && kind != SceneNodeKind::kRoundedRect) {
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
                                     node.destination != original.destination || node.centered != original.centered ||
                                         node.radius != original.radius || node.stroke_width != original.stroke_width);
}

void UpdateNodeAppearanceDirty(SceneNodeData& node, const SceneNodeData& original) {
    detail::UpdateScenePropertyDirty(
        node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE,
        node.color != original.color || node.stroke_color != original.stroke_color || node.opacity != original.opacity);
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

uint16_t BuildOrderedNodeSlots(const SceneState& state, uint16_t (&ordered)[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]) {
    uint16_t count = 0U;
    for (uint16_t slot = 0U; slot < state.nodes.size(); ++slot) {
        if (!state.nodes[slot].occupied) {
            continue;
        }
        uint16_t insertion = count;
        while (insertion > 0U && state.nodes[ordered[insertion - 1U]].order > state.nodes[slot].order) {
            ordered[insertion] = ordered[insertion - 1U];
            --insertion;
        }
        ordered[insertion] = slot;
        ++count;
    }
    return count;
}

uint16_t BuildOrderedContainerSlots(const SceneState& state, uint16_t (&ordered)[MICROPIXEL_GRAPHICS_MAX_CONTAINERS]) {
    uint16_t count = 0U;
    for (uint16_t slot = 1U; slot < state.containers.size(); ++slot) {
        if (!state.containers[slot].occupied) {
            continue;
        }
        uint16_t insertion = count;
        while (insertion > 0U && state.containers[ordered[insertion - 1U]].order > state.containers[slot].order) {
            ordered[insertion] = ordered[insertion - 1U];
            --insertion;
        }
        ordered[insertion] = slot;
        ++count;
    }
    return count;
}

uint16_t WireSiblingOrder(const SceneState& state, uint32_t child_order) {
    uint16_t order = 0U;
    for (uint16_t slot = 0U; slot < state.nodes.size(); ++slot) {
        order += state.nodes[slot].occupied && state.nodes[slot].order < child_order ? 1U : 0U;
    }
    for (uint16_t slot = 1U; slot < state.containers.size(); ++slot) {
        order += state.containers[slot].occupied && state.containers[slot].order < child_order ? 1U : 0U;
    }
    return order;
}

int32_t EncodeAndSubmit(SceneState& state, bool keyframe) {
    SceneWriter writer;
    uint16_t ordered_node_slots[MICROPIXEL_GRAPHICS_MAX_SCENE_NODES]{};
    uint16_t ordered_container_slots[MICROPIXEL_GRAPHICS_MAX_CONTAINERS]{};
    uint16_t container_wire_ids[MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U]{};
    const uint16_t ordered_node_count = keyframe ? BuildOrderedNodeSlots(state, ordered_node_slots) : 0U;
    const uint16_t ordered_container_count = keyframe ? BuildOrderedContainerSlots(state, ordered_container_slots) : 0U;
    if (keyframe && (ordered_node_count != state.node_count || ordered_container_count != state.container_count)) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    if (keyframe) {
        for (uint16_t wire_index = 0U; wire_index < ordered_container_count; ++wire_index) {
            container_wire_ids[ordered_container_slots[wire_index]] = static_cast<uint16_t>(wire_index + 1U);
        }
    }
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
    const uint16_t container_records = keyframe ? state.container_count : state.undo_container_count;
    for (uint16_t record_index = 0U; record_index < container_records; ++record_index) {
        const uint16_t id = keyframe ? ordered_container_slots[record_index] : state.container_undo[record_index].id;
        const SceneContainerData& container = state.containers[id];
        if (!container.occupied) {
            continue;
        }
        const uint32_t mask = keyframe ? kContainerMask : container.dirty;
        if (mask == 0U) {
            continue;
        }
        const detail::PhysicalRect clip = container.clip.empty()
                                              ? detail::PhysicalRect{}
                                              : detail::MapSceneRect(state.display, container.clip.x, container.clip.y,
                                                                     container.clip.width, container.clip.height);
        const uint16_t wire_id = keyframe ? container_wire_ids[id] : container.wire_id;
        const uint16_t parent_wire_id =
            container.parent_id == 0U
                ? 0U
                : (keyframe ? container_wire_ids[container.parent_id] : state.containers[container.parent_id].wire_id);
        if (wire_id == 0U || wire_id == UINT16_MAX || parent_wire_id == UINT16_MAX ||
            !writer.Add(micropixel_graphics_scene_container_record_t{
                .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_CONTAINER,
                           .size = sizeof(micropixel_graphics_scene_container_record_t)},
                .container_id = wire_id,
                .parent_container_id = parent_wire_id,
                .property_mask = mask,
                .clip_x = clip.x,
                .clip_y = clip.y,
                .width = clip.width,
                .height = clip.height,
                .translate_x = detail::MapSceneVectorX(state.display, container.translation.x),
                .translate_y = detail::MapSceneVectorY(state.display, container.translation.y),
                .z_order = container.z_order,
                .opacity = container.opacity,
                .visible = static_cast<uint8_t>(container.visible ? 1U : 0U),
                .sibling_order = WireSiblingOrder(state, container.order),
                .reserved0 = 0U,
            })) {
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
    }
    const uint16_t node_records = keyframe ? state.node_count : state.undo_node_count;
    for (uint16_t record_index = 0U; record_index < node_records; ++record_index) {
        const uint16_t id = keyframe ? ordered_node_slots[record_index] : state.node_undo[record_index].id;
        const SceneNodeData& node = state.nodes[id];
        if (!node.occupied) {
            continue;
        }
        const uint32_t mask = keyframe ? FullMask(node.kind) : node.dirty;
        if (mask == 0U) {
            continue;
        }
        micropixel_graphics_scene_node_header_t header{
            .record = {.opcode = static_cast<uint16_t>(node.kind), .size = 0U},
            .node_id = keyframe ? record_index : node.wire_id,
            .container_id = 0U,
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
        } else if (node.kind == SceneNodeKind::kRoundedRect) {
            auto value = micropixel_graphics_scene_rounded_rect_record_t{
                .node = header,
                .x = destination.x,
                .y = destination.y,
                .width = destination.width,
                .height = destination.height,
                .fill_rgb888 = node.color.rgb888(),
                .stroke_rgb888 = node.stroke_color.rgb888(),
                .radius = static_cast<uint32_t>(detail::MapSceneVectorX(state.display, node.radius)),
                .stroke_width = static_cast<uint32_t>(detail::MapSceneVectorX(state.display, node.stroke_width)),
                .opacity = node.opacity,
                .reserved0 = {},
            };
            value.node.record.size = sizeof(value);
            added = writer.Add(value);
        } else if (node.kind == SceneNodeKind::kSprite) {
            const detail::PhysicalRect source = detail::MapTextureRect(
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
                    .font = node.font,
                    .text_length = static_cast<uint16_t>(
                        (mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U ? node.text_length : 0U),
                },
                node.text);
        }
        if (!added) {
            return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
        }
        if (keyframe) {
            const uint16_t parent_wire_id =
                node.parent_container_id == 0U ? 0U : container_wire_ids[node.parent_container_id];
            if (!writer.Add(micropixel_graphics_scene_node_link_record_t{
                    .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK,
                               .size = sizeof(micropixel_graphics_scene_node_link_record_t)},
                    .node_id = record_index,
                    .parent_container_id = parent_wire_id,
                    .sibling_order = WireSiblingOrder(state, node.order),
                    .reserved0 = 0U,
                })) {
                return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
            }
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
    if (keyframe) {
        for (uint16_t wire_id = 0U; wire_id < ordered_node_count; ++wire_id) {
            const SceneNodeData& node = state.nodes[ordered_node_slots[wire_id]];
            if (node.kind != SceneNodeKind::kSpriteBatch) {
                continue;
            }
            if (!writer.AddInstances(
                    micropixel_graphics_scene_batch_instances_record_t{
                        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES, .size = 0U},
                        .batch_node_id = wire_id,
                        .first_instance = 0U,
                        .instance_count = node.batch_capacity,
                        .reserved0 = 0U,
                        .property_mask = kInstanceMask,
                    },
                    state.instances.data() + node.batch_instance_offset, node, state.display)) {
                return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
            }
        }
    } else {
        uint16_t changed_index = 0U;
        while (changed_index < changed_instance_count) {
            const uint16_t scene_instance = changed_instances[changed_index];
            const SceneNodeData* batch = nullptr;
            for (uint16_t slot = 0U; slot < state.nodes.size(); ++slot) {
                const SceneNodeData& candidate = state.nodes[slot];
                if (candidate.occupied && candidate.kind == SceneNodeKind::kSpriteBatch &&
                    scene_instance >= candidate.batch_instance_offset &&
                    scene_instance < candidate.batch_instance_offset + candidate.batch_capacity) {
                    batch = &candidate;
                    break;
                }
            }
            if (batch == nullptr || batch->wire_id == UINT16_MAX) {
                return MICROPIXEL_STATUS_INTERNAL;
            }
            const uint16_t batch_end = static_cast<uint16_t>(batch->batch_instance_offset + batch->batch_capacity);
            const uint16_t local_instance = static_cast<uint16_t>(scene_instance - batch->batch_instance_offset);
            const uint32_t mask = state.instances[scene_instance].dirty;
            uint16_t count = 1U;
            while (changed_index + count < changed_instance_count &&
                   changed_instances[changed_index + count] == scene_instance + count &&
                   changed_instances[changed_index + count] < batch_end &&
                   state.instances[scene_instance + count].dirty == mask) {
                ++count;
            }
            if (!writer.AddInstances(
                    micropixel_graphics_scene_batch_instances_record_t{
                        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES, .size = 0U},
                        .batch_node_id = batch->wire_id,
                        .first_instance = local_instance,
                        .instance_count = count,
                        .reserved0 = 0U,
                        .property_mask = mask,
                    },
                    state.instances.data() + scene_instance, *batch, state.display)) {
                return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
            }
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
        .container_count = state.container_count,
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
            for (uint16_t wire_index = 0U; wire_index < ordered_container_count; ++wire_index) {
                SceneContainerData& container = state.containers[ordered_container_slots[wire_index]];
                container.wire_id = static_cast<uint16_t>(wire_index + 1U);
                container.dirty = 0U;
            }
            for (uint16_t wire_id = 0U; wire_id < ordered_node_count; ++wire_id) {
                SceneNodeData& node = state.nodes[ordered_node_slots[wire_id]];
                node.wire_id = wire_id;
                node.dirty = 0U;
            }
            for (uint16_t id = 0U; id < state.batch_instance_count; ++id) {
                state.instances[id].dirty = 0U;
            }
        } else {
            for (uint16_t index = 0U; index < state.undo_container_count; ++index) {
                state.containers[state.container_undo[index].id].dirty = 0U;
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

bool NodeHandle::valid() const {
    return state_ == &scene_storage && scene_active && state_->NodeValid(id_, generation_);
}

void NodeHandle::Destroy(SceneUpdate& update) {
    if (state_ == nullptr) {
        return;
    }
    ValidateUpdate(state_, update);
    state_->DestroyNode(id_, generation_);
}

bool Container::valid() const {
    return state_ == &scene_storage && scene_active &&
           (id_ == 0U ? generation_ == 0U : state_->ContainerValid(id_, generation_));
}

Point Container::SceneTranslation() const {
    if (!valid()) {
        runtime::Panic("scene.container.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    int64_t x = 0;
    int64_t y = 0;
    uint16_t container_id = id_;
    for (uint16_t depth = 0U; container_id != 0U && depth < MICROPIXEL_GRAPHICS_MAX_CONTAINERS; ++depth) {
        const SceneContainerData& container = state_->containers[container_id];
        x += container.translation.x;
        y += container.translation.y;
        container_id = container.parent_id;
    }
    if (container_id != 0U || x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        runtime::Panic("scene.container.transform", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return {static_cast<int32_t>(x), static_cast<int32_t>(y)};
}

Point Container::ToScene(Point local) const {
    const Point translation = SceneTranslation();
    const int64_t x = static_cast<int64_t>(local.x) + translation.x;
    const int64_t y = static_cast<int64_t>(local.y) + translation.y;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        runtime::Panic("scene.container.to_scene", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return {static_cast<int32_t>(x), static_cast<int32_t>(y)};
}

Point Container::ToLocal(Point scene) const {
    const Point translation = SceneTranslation();
    const int64_t x = static_cast<int64_t>(scene.x) - translation.x;
    const int64_t y = static_cast<int64_t>(scene.y) - translation.y;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
        runtime::Panic("scene.container.to_local", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return {static_cast<int32_t>(x), static_cast<int32_t>(y)};
}

void ContainerNode::Destroy(SceneUpdate& update) {
    if (state_ == nullptr) {
        return;
    }
    ValidateUpdate(state_, update);
    state_->DestroyContainer(id_, generation_);
}

Result<void> ContainerNode::Destroy() {
    if (!valid()) {
        return {};
    }
    if (state_->update_active) {
        runtime::Panic("scene.update.concurrent", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    state_->BeginTransaction();
    SceneUpdate update{state_};
    Destroy(update);
    return update.Present();
}

void NodeHandle::SetVisible(SceneUpdate& update, bool visible) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.visible == visible) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.visible = visible;
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY,
                                     node.visible != original.visible);
}

void ContainerNode::SetClip(SceneUpdate& update, Rect clip) {
    ValidateUpdate(state_, update);
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.clip == clip) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.clip = clip;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP,
                                     container.clip != original.clip);
}

void ContainerNode::SetTranslation(SceneUpdate& update, Point translation) {
    ValidateUpdate(state_, update);
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.translation == translation) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.translation = translation;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION,
                                     container.translation != original.translation);
}

void ContainerNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.opacity == opacity) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.opacity = opacity;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE,
                                     container.opacity != original.opacity || container.visible != original.visible);
}

void ContainerNode::SetVisible(SceneUpdate& update, bool visible) {
    ValidateUpdate(state_, update);
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.visible == visible) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.visible = visible;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE,
                                     container.opacity != original.opacity || container.visible != original.visible);
}

void ContainerNode::SetZOrder(SceneUpdate& update, int16_t z_order) {
    ValidateUpdate(state_, update);
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.z_order == z_order) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.z_order = z_order;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER,
                                     container.z_order != original.z_order);
}

void ShapeNode::SetRect(SceneUpdate& update, Rect rect) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == rect) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = rect;
    UpdateNodeGeometryDirty(node, original);
}

void ShapeNode::SetColor(SceneUpdate& update, Color color) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void ShapeNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void RoundedRectNode::SetRect(SceneUpdate& update, Rect rect) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == rect) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = rect;
    UpdateNodeGeometryDirty(node, original);
}

void RoundedRectNode::SetFillColor(SceneUpdate& update, Color color) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void RoundedRectNode::SetStrokeColor(SceneUpdate& update, Color color) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.stroke_color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.stroke_color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void RoundedRectNode::SetRadius(SceneUpdate& update, uint32_t radius) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.radius == radius) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.radius = radius;
    UpdateNodeGeometryDirty(node, original);
}

void RoundedRectNode::SetStrokeWidth(SceneUpdate& update, uint32_t stroke_width) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.stroke_width == stroke_width) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.stroke_width = stroke_width;
    UpdateNodeGeometryDirty(node, original);
}

void RoundedRectNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SpriteNode::SetDestination(SceneUpdate& update, Rect destination) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == destination) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = destination;
    UpdateNodeGeometryDirty(node, original);
}

void SpriteNode::SetSource(SceneUpdate& update, Rect source) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SurfaceNode::SetDestination(SceneUpdate& update, Rect destination) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == destination) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = destination;
    UpdateNodeGeometryDirty(node, original);
}

void SurfaceNode::SetSource(SceneUpdate& update, Rect source) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.source == source) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.source = source;
    UpdateNodeContentDirty(node, original);
}

void SurfaceNode::SetOpacity(SceneUpdate& update, uint8_t opacity) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void LabelNode::SetPosition(SceneUpdate& update, Point position) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void LabelNode::SetFont(SceneUpdate& update, SystemFont font) {
    ValidateUpdate(state_, update);
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
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
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SpriteBatch::SetInstance(SceneUpdate& update, uint16_t instance_id, const SpriteInstance& instance) {
    ValidateUpdate(state_, update);
    const SceneNodeData& batch = state_->Node(id_, generation_);
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
    const SceneNodeData& batch = state_->Node(id_, generation_);
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

Scene::Scene(CapabilityToken, const SceneDescriptor& descriptor) : Container(&scene_storage, 0U, 0U) {
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

Scene::Scene(Scene&& other) noexcept : Container(other.state_, 0U, 0U) { other.state_ = nullptr; }

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

ContainerNode Container::CreateContainer(const ContainerProperties& properties) {
    ValidateHandle(state_);
    const uint16_t id = state_->AllocateContainer(*this, properties);
    return ContainerNode{state_, id, state_->containers[id].generation};
}

ShapeNode Container::CreateShape(Rect rect, Color color, uint8_t opacity) {
    ValidateHandle(state_);
    const uint16_t id = state_->AllocateNode(*this);
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kShape;
    node.dirty = FullMask(SceneNodeKind::kShape);
    node.visible = true;
    node.destination = rect;
    node.color = color;
    node.opacity = opacity;
    return ShapeNode{state_, id, node.generation};
}

RoundedRectNode Container::CreateRoundedRect(Rect rect, const RoundedRectStyle& style) {
    ValidateHandle(state_);
    const uint16_t id = state_->AllocateNode(*this);
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kRoundedRect;
    node.dirty = FullMask(SceneNodeKind::kRoundedRect);
    node.visible = true;
    node.destination = rect;
    node.color = style.fill;
    node.stroke_color = style.stroke;
    node.radius = style.radius;
    node.stroke_width = style.stroke_width;
    node.opacity = style.opacity;
    return RoundedRectNode{state_, id, node.generation};
}

SpriteNode Container::CreateSprite(const Texture& texture, Rect destination, Rect source, uint8_t opacity) {
    ValidateHandle(state_);
    if (!texture.valid()) {
        runtime::Panic("scene.sprite.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->AllocateNode(*this);
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kSprite;
    node.dirty = FullMask(SceneNodeKind::kSprite);
    node.visible = true;
    node.destination = destination;
    node.color = Color::Black();
    node.opacity = opacity;
    node.texture = texture.handle_;
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    node.source = source;
    return SpriteNode{state_, id, node.generation};
}

SurfaceNode Container::CreateSurfaceNode(const StreamingTexture& surface, Rect destination, Rect source,
                                         uint8_t opacity) {
    ValidateHandle(state_);
    if (!surface.valid()) {
        runtime::Panic("scene.surface.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->AllocateNode(*this);
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kSprite;
    node.dirty = FullMask(SceneNodeKind::kSprite);
    node.visible = true;
    node.destination = destination;
    node.color = Color::Black();
    node.opacity = opacity;
    node.texture = surface.texture_.handle_;
    node.texture_logical_width = surface.texture_.width_;
    node.texture_logical_height = surface.texture_.height_;
    node.texture_physical_width = surface.texture_.physical_width_;
    node.texture_physical_height = surface.texture_.physical_height_;
    node.source = source;
    return SurfaceNode{state_, id, node.generation};
}

SpriteBatch Container::CreateSpriteBatch(const Texture& texture, uint16_t capacity, uint8_t opacity) {
    if (!texture.valid()) {
        runtime::Panic("scene.batch.texture", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SpriteBatch batch = CreateSpriteBatchInternal(texture.handle_, capacity, opacity);
    SceneNodeData& node = state_->Node(batch.id_, batch.generation_);
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    return batch;
}

SpriteBatch Container::CreateSpriteBatch(uint16_t capacity, uint8_t opacity) {
    return CreateSpriteBatchInternal(0U, capacity, opacity);
}

SpriteBatch Container::CreateSpriteBatchInternal(uint32_t texture, uint16_t capacity, uint8_t opacity) {
    ValidateHandle(state_);
    if (capacity == 0U || capacity > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES ||
        static_cast<uint32_t>(state_->node_count) + state_->batch_instance_count + capacity >
            MICROPIXEL_GRAPHICS_MAX_SCENE_NODES ||
        static_cast<uint32_t>(state_->batch_instance_count) + capacity > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES) {
        runtime::Panic("scene.batch.create", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    uint8_t batch_count = 0U;
    for (uint16_t index = 0U; index < state_->nodes.size(); ++index) {
        batch_count +=
            state_->nodes[index].occupied && state_->nodes[index].kind == SceneNodeKind::kSpriteBatch ? 1U : 0U;
    }
    if (batch_count >= MICROPIXEL_GRAPHICS_MAX_SPRITE_BATCHES) {
        runtime::Panic("scene.batches.full", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    const uint16_t id = state_->AllocateNode(*this);
    const uint16_t instance_offset = state_->batch_instance_count;
    state_->batch_instance_count += capacity;
    if (state_->instances.size() < state_->batch_instance_count) {
        state_->instances.resize(state_->batch_instance_count);
    }
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kSpriteBatch;
    node.dirty = FullMask(SceneNodeKind::kSpriteBatch);
    node.visible = true;
    node.color = Color::Black();
    node.opacity = opacity;
    node.texture = texture;
    node.batch_capacity = capacity;
    node.batch_instance_offset = instance_offset;
    for (uint16_t instance = 0U; instance < capacity; ++instance) {
        if (state_->update_active) {
            state_->RememberInstance(static_cast<uint16_t>(instance_offset + instance));
        }
        state_->instances[instance_offset + instance] = {.dirty = kInstanceMask, .value = {}};
    }
    return SpriteBatch{state_, id, node.generation, capacity};
}

LabelNode Container::CreateLabel(Point position, const char* text, Color color, SystemFont font, bool centered) {
    ValidateHandle(state_);
    const uint16_t id = state_->AllocateNode(*this);
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kLabel;
    node.dirty = FullMask(SceneNodeKind::kLabel);
    node.visible = true;
    node.centered = centered;
    node.destination = {.x = position.x, .y = position.y};
    node.color = color;
    node.opacity = 255U;
    node.font = static_cast<uint16_t>(font);
    const uint16_t length = TextLength(text);
    Copy(node.text, text, length);
    node.text[length] = '\0';
    node.text_length = length;
    return LabelNode{state_, id, node.generation};
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
    bool stale_state = false;
    if (status == MICROPIXEL_STATUS_STALE_STATE) {
        stale_state = true;
        state_->valid = false;
        status = EncodeAndSubmit(*state_, true);
    }
    if (status == MICROPIXEL_STATUS_OK) {
        state_->CommitTransaction();
    } else {
        state_->RollbackTransaction();
        if (stale_state) {
            state_->valid = false;
        }
    }
    active_ = false;
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(StatusError(status))};
}

uint16_t Scene::node_count() const {
    ValidateHandle(state_);
    return state_->node_count;
}

Scene Renderer::CreateScene(Color background) const {
    const detail::DisplayTransform& display = detail::CurrentDisplayTransform();
    return CreateScene(
        {.logical_width = display.logical_width, .logical_height = display.logical_height, .background = background});
}

Scene Renderer::CreateScene(const SceneDescriptor& descriptor) const {
    return Scene{Scene::CapabilityToken{}, descriptor};
}

}  // namespace micropixel
