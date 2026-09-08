#include <stdint.h>

#include <new>

#include "abi/micropixel_abi.h"
#include "runtime/display_context.hpp"
#include "runtime/display_transform.hpp"
#include "runtime/panic.hpp"
#include "runtime/scene_array.hpp"
#include "runtime/scene_delta.hpp"
#include "runtime/service_binding.hpp"
#include "runtime/texture_state.hpp"
#include "sdk/graphics.hpp"
#include "sdk/resources.hpp"

namespace micropixel {
namespace {

constexpr uint32_t kBaseMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY;
constexpr uint32_t kCommonMask = kBaseMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY;
constexpr uint32_t kContainerMask =
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER |
    MICROPIXEL_GRAPHICS_SCENE_CONTAINER_STRUCTURE | MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;
constexpr uint16_t kNoUndoSlot = UINT16_MAX;
uint32_t next_handle_identity = 1U;
uint32_t NextIdentity() {
    if (next_handle_identity == UINT32_MAX) {
        runtime::Panic("scene.identity.exhausted", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    return next_handle_identity++;
}

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
    uint32_t texture_handle{};
    uint32_t texture_logical_width{};
    uint32_t texture_logical_height{};
    uint32_t texture_physical_width{};
    uint32_t texture_physical_height{};
    Rect source{};
    micropixel_font_handle_t font_handle{};
    uint16_t text_length{};
    uint16_t batch_capacity{};
    uint16_t batch_instance_offset{};
    // Offset of text_length bytes plus NUL in SceneState::text_arena. Stable
    // for as long as the node or an undo copy of it refers to the text.
    uint32_t text_offset{};
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
    bool cache_content{};
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
    const uint32_t max_text_bytes = runtime::LoadGraphicsLimits().max_text_bytes;
    uint16_t length = 0U;
    while (length <= max_text_bytes && text[length] != '\0') {
        ++length;
    }
    if (length == 0U || length > max_text_bytes) {
        runtime::Panic("scene.text.length", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return length;
}

Error StatusError(int32_t status) { return runtime::ErrorFromStatus(status); }

class SceneWriter final {
   public:
    // Effective message capacity: the static buffer clamped to Host policy.
    [[nodiscard]] static uint32_t Capacity() { return runtime::LoadGraphicsLimits().max_scene_bytes; }

    template <typename Value>
    bool Add(const Value& value) {
        if (size_ > Capacity() || sizeof(Value) > Capacity() - size_) {
            return false;
        }
        Copy(scene_wire + size_, &value, sizeof(Value));
        size_ += sizeof(Value);
        ++records_;
        return true;
    }

    bool AddText(micropixel_graphics_scene_text_record_t value, const char* text) {
        const uint32_t size = (sizeof(value) + value.text_length + 3U) & ~3U;
        if (size_ > Capacity() || size > Capacity() - size_) {
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
        if (size_ > Capacity() || size > Capacity() - size_ || size > UINT16_MAX) {
            return false;
        }
        value.record.size = static_cast<uint16_t>(size);
        Copy(scene_wire + size_, &value, sizeof(value));
        uint32_t output = size_ + sizeof(value);
        for (uint16_t index = 0U; index < value.instance_count; ++index) {
            const SpriteInstance& instance = instances[index].value;
            const detail::PhysicalRect destination =
                batch.texture_handle == 0U
                    ? detail::MapSceneRect(display, instance.destination.x, instance.destination.y,
                                           instance.destination.width, instance.destination.height)
                    : detail::MapSceneSizedRect(display, instance.destination.x, instance.destination.y,
                                                instance.destination.width, instance.destination.height);
            const detail::PhysicalRect source =
                batch.texture_handle == 0U
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
    alignas(4) static uint8_t scene_wire[runtime::limits::kMaxSceneBytes];
};

alignas(4) uint8_t SceneWriter::scene_wire[runtime::limits::kMaxSceneBytes]{};
micropixel_service_info_t graphics_scene_service{};
bool graphics_scene_service_open{};

}  // namespace

class SceneState final {
   public:
    bool Reset(const SceneDescriptor& descriptor) {
        if (!containers.Reserve(1) || !container_undo_slot.Reserve(1) || !container_undo.Reserve(1) ||
            !container_scratch.Reserve(1) || !container_wire_ids.Reserve(1) || !container_subtree.Reserve(1))
            return false;
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
        text_arena.clear();
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
        pending_changes = false;
        undo_node_count = 0U;
        undo_instance_count = 0U;
        undo_container_count = 0U;
        background_saved = false;
        next_order = 0U;
        return true;
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
        if (parent.state_ != this || (parent.id_ == 0U ? parent.generation_ != root_identity
                                                       : !ContainerValid(parent.id_, parent.generation_))) {
            runtime::Panic("scene.container.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        return parent.id_;
    }

    // Nodes and batch instances share the Host's uint16 draw-operation space.
    [[nodiscard]] bool ItemsAvailable(uint32_t additional) const {
        return static_cast<uint32_t>(node_count) + batch_instance_count + additional <= runtime::limits::kMaxSceneItems;
    }

    Result<uint16_t> AllocateNode(const Container& parent) {
        if (!ItemsAvailable(1U) || next_handle_identity == UINT32_MAX) {
            return unexpected(Error{ErrorCode::kResourceExhausted});
        }
        const uint16_t parent_id = ParentId(parent);
        uint16_t id = 0U;
        while (id < nodes.size() && nodes[id].occupied) {
            ++id;
        }
        if (id >= runtime::limits::kMaxSceneItems || next_order == UINT32_MAX) {
            return unexpected(Error{ErrorCode::kResourceExhausted});
        }
        if (id == nodes.size()) {
            const size_t count = nodes.size() + 1;
            if (!nodes.Reserve(count) || !node_undo_slot.Reserve(count) || !node_undo.Reserve(count) ||
                !node_scratch.Reserve(count))
                return unexpected(Error{ErrorCode::kResourceExhausted});
            nodes.emplace_back();
            node_undo_slot.push_back(kNoUndoSlot);
        }
        if (update_active) {
            RememberNode(id);
        }
        const uint32_t generation = NextIdentity();
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

    Result<uint16_t> AllocateContainer(const Container& parent, const ContainerProperties& properties) {
        // Container ids are uint16 with 0 reserved for the root.
        if (container_count >= runtime::limits::kMaxSceneItems - 1U || next_order == UINT32_MAX ||
            next_handle_identity == UINT32_MAX) {
            return unexpected(Error{ErrorCode::kResourceExhausted});
        }
        const uint16_t parent_id = ParentId(parent);
        uint16_t id = 1U;
        while (id < containers.size() && containers[id].occupied) {
            ++id;
        }
        if (id >= runtime::limits::kMaxSceneItems) {
            return unexpected(Error{ErrorCode::kResourceExhausted});
        }
        if (id == containers.size()) {
            const size_t count = containers.size() + 1;
            if (!containers.Reserve(count) || !container_undo_slot.Reserve(count) || !container_undo.Reserve(count) ||
                !container_scratch.Reserve(count) || !container_wire_ids.Reserve(count) ||
                !container_subtree.Reserve(count))
                return unexpected(Error{ErrorCode::kResourceExhausted});
            containers.emplace_back();
            container_undo_slot.push_back(kNoUndoSlot);
        }
        if (update_active) {
            RememberContainer(id);
        }
        const uint32_t generation = NextIdentity();
        containers[id] = {.dirty = kContainerMask,
                          .clip = properties.clip,
                          .translation = properties.translation,
                          .z_order = properties.z_order,
                          .opacity = properties.opacity,
                          .visible = properties.visible,
                          .cache_content = properties.cache_content,
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

    void BeginFrame() {
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
        // Reserved alongside containers, so this never allocates.
        container_subtree.assign(containers.size(), 0U);
        uint8_t* subtree = container_subtree.data();
        subtree[id] = 1U;
        for (uint16_t pass = 0U; pass < container_count; ++pass) {
            bool changed = false;
            for (uint16_t container_id = 1U; container_id < containers.size(); ++container_id) {
                if (containers[container_id].occupied && !subtree[container_id] &&
                    subtree[containers[container_id].parent_id]) {
                    subtree[container_id] = 1U;
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

    void AcceptFrame() {
        update_active = false;
        pending_changes = false;
    }

    [[nodiscard]] const char* Text(const SceneNodeData& node) const { return text_arena.data() + node.text_offset; }

    // Appends `length` bytes plus NUL and points `node` at them. Old text stays
    // in place (undo copies may still refer to it); when the arena is full the
    // garbage is squeezed out first and the arena only grows if live text plus
    // the new run do not fit. False means out of memory; nothing changed.
    [[nodiscard]] bool StoreText(SceneNodeData& node, const char* text, uint16_t length) {
        const size_t needed = static_cast<size_t>(length) + 1U;
        if (text_arena.size() + needed > text_arena.capacity()) {
            CompactText();
        }
        const size_t offset = text_arena.size();
        if (!text_arena.Reserve(offset + needed)) {
            return false;
        }
        text_arena.resize(offset + needed);
        Copy(text_arena.data() + offset, text, length);
        text_arena[offset + length] = '\0';
        node.text_offset = static_cast<uint32_t>(offset);
        node.text_length = length;
        return true;
    }

    // In-place compaction keeping every run referenced by a live label or by an
    // undo copy. Runs are slid down in ascending offset order, so a run is never
    // overwritten before it was moved. Quadratic in the number of label runs;
    // only reached when the arena is full.
    void CompactText() {
        // A label whose run is not inside the arena yet (CreateLabel before
        // its first StoreText) owns no bytes.
        const size_t used = text_arena.size();
        const auto label = [used](const SceneNodeData& node) {
            return node.occupied && node.kind == SceneNodeKind::kLabel &&
                   static_cast<size_t>(node.text_offset) + node.text_length < used;
        };
        uint32_t write = 0U;
        uint32_t scan = 0U;
        while (true) {
            uint32_t best = UINT32_MAX;
            uint16_t best_length = 0U;
            for (size_t index = 0U; index < nodes.size(); ++index) {
                const SceneNodeData& node = nodes[index];
                if (label(node) && node.text_offset >= scan && node.text_offset < best) {
                    best = node.text_offset;
                    best_length = node.text_length;
                }
            }
            for (uint16_t index = 0U; index < undo_node_count; ++index) {
                const SceneNodeData& node = node_undo[index].value;
                if (label(node) && node.text_offset >= scan && node.text_offset < best) {
                    best = node.text_offset;
                    best_length = node.text_length;
                }
            }
            if (best == UINT32_MAX) {
                break;
            }
            if (best != write) {
                Copy(text_arena.data() + write, text_arena.data() + best, static_cast<uint32_t>(best_length) + 1U);
            }
            for (size_t index = 0U; index < nodes.size(); ++index) {
                if (label(nodes[index]) && nodes[index].text_offset == best) {
                    nodes[index].text_offset = write;
                }
            }
            for (uint16_t index = 0U; index < undo_node_count; ++index) {
                if (label(node_undo[index].value) && node_undo[index].value.text_offset == best) {
                    node_undo[index].value.text_offset = write;
                }
            }
            write += static_cast<uint32_t>(best_length) + 1U;
            scan = best + 1U;
        }
        text_arena.resize(write);
    }

    void BeginPending() {
        if (!update_active) {
            BeginFrame();
            pending_changes = true;
        }
    }

    runtime::SceneArray<SceneNodeData> nodes{};
    runtime::SceneArray<SceneContainerData> containers{};
    runtime::SceneArray<SceneInstanceData> instances{};
    runtime::SceneArray<SceneNodeUndo> node_undo{};
    runtime::SceneArray<SceneInstanceUndo> instance_undo{};
    runtime::SceneArray<SceneContainerUndo> container_undo{};
    runtime::SceneArray<uint16_t> node_undo_slot{};
    runtime::SceneArray<uint16_t> instance_undo_slot{};
    runtime::SceneArray<uint16_t> container_undo_slot{};
    runtime::SceneArray<char> text_arena{};
    // Encoder scratch, reserved together with the arrays they index so that
    // Submit never allocates.
    runtime::SceneArray<uint16_t> node_scratch{};
    runtime::SceneArray<uint16_t> container_scratch{};
    runtime::SceneArray<uint16_t> container_wire_ids{};
    runtime::SceneArray<uint8_t> container_subtree{};
    runtime::SceneArray<uint16_t> instance_scratch{};
    SceneState* next_live{};
    Color background_color{Color::Black()};
    Color background_undo{Color::Black()};
    uint16_t node_count{};
    uint16_t batch_instance_count{};
    uint16_t container_count{};
    uint32_t logical_width{};
    uint32_t logical_height{};
    detail::DisplayTransform display{};
    uint32_t generation{};
    uint32_t root_identity{};
    uint32_t revision{};
    uint64_t texture_revision{};
    uint32_t next_order{};
    bool background_dirty{};
    bool background_dirty_undo{};
    uint16_t undo_node_count{};
    uint16_t undo_instance_count{};
    uint16_t undo_container_count{};
    bool background_saved{};
    bool valid{};
    bool update_active{};
    bool pending_changes{};
};

namespace {

SceneState scene_storage __attribute__((no_destroy));
bool scene_active{};
SceneState* live_scenes{};
SceneState* displayed_scene{};
uint32_t wire_generation{};

bool StateAlive(const SceneState* state) {
    if (state == nullptr) return false;
    if (state == &scene_storage && scene_active) return true;
    for (SceneState* candidate = live_scenes; candidate != nullptr; candidate = candidate->next_live) {
        if (candidate == state) return true;
    }
    return false;
}

uint32_t FullMask(SceneNodeKind kind) {
    uint32_t mask =
        (kind == SceneNodeKind::kSpriteBatch ? kBaseMask : kCommonMask) | MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
    if (kind != SceneNodeKind::kShape && kind != SceneNodeKind::kRoundedRect) {
        mask |= MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT;
    }
    return mask;
}

bool SameText(const SceneState& state, const SceneNodeData& left, const SceneNodeData& right) {
    if (left.text_length != right.text_length) {
        return false;
    }
    if (left.text_offset == right.text_offset) {
        return true;
    }
    const char* left_text = state.Text(left);
    const char* right_text = state.Text(right);
    for (uint16_t index = 0U; index < left.text_length; ++index) {
        if (left_text[index] != right_text[index]) {
            return false;
        }
    }
    return true;
}

bool SameTexture(const SceneNodeData& left, const SceneNodeData& right) {
    return left.texture_handle == right.texture_handle && left.texture_logical_width == right.texture_logical_width &&
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

void UpdateNodeContentDirty(const SceneState& state, SceneNodeData& node, const SceneNodeData& original) {
    bool changed = !SameTexture(node, original);
    if (node.kind == SceneNodeKind::kSprite) {
        changed = changed || node.source != original.source;
    } else if (node.kind == SceneNodeKind::kLabel) {
        changed = node.font_handle != original.font_handle || !SameText(state, node, original);
    }
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT, changed);
}

void ValidateHandle(SceneState* state) {
    if (!StateAlive(state)) {
        runtime::Panic("scene.handle.stale", MICROPIXEL_STATUS_CLOSED);
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

uint16_t BuildOrderedNodeSlots(const SceneState& state, uint16_t* ordered) {
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

uint16_t BuildOrderedContainerSlots(const SceneState& state, uint16_t* ordered) {
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
    keyframe = keyframe || displayed_scene != &state || state.texture_revision != runtime::texture_revision;
    SceneWriter writer;
    // Scratch was reserved when the arrays it indexes grew; assign() only traps
    // if that contract is broken.
    state.node_scratch.assign(state.nodes.size(), 0U);
    state.container_scratch.assign(state.containers.size(), 0U);
    state.container_wire_ids.assign(state.containers.size(), 0U);
    uint16_t* ordered_node_slots = state.node_scratch.data();
    uint16_t* ordered_container_slots = state.container_scratch.data();
    uint16_t* container_wire_ids = state.container_wire_ids.data();
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
                .clip_width = clip.width,
                .clip_height = clip.height,
                .translate_x = detail::MapSceneVectorX(state.display, container.translation.x),
                .translate_y = detail::MapSceneVectorY(state.display, container.translation.y),
                .z_order = container.z_order,
                .opacity = container.opacity,
                .visible = static_cast<uint8_t>(container.visible ? 1U : 0U),
                .sibling_order = WireSiblingOrder(state, container.order),
                .flags = static_cast<uint16_t>(
                    container.cache_content ? MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAG_CACHED_CONTENT : 0U),
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
            .flags = static_cast<uint8_t>((node.visible ? MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE : 0U) |
                                          (node.centered ? MICROPIXEL_GRAPHICS_SCENE_TEXT_CENTERED : 0U)),
            .reserved0 = 0U,
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
                .texture_handle = runtime::TextureSnapshot(node.texture_handle),
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
                .texture_handle = runtime::TextureSnapshot(node.texture_handle),
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
                    .font_handle = node.font_handle,
                    .text_length = static_cast<uint16_t>(
                        (mask & MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT) != 0U ? node.text_length : 0U),
                    .reserved0 = 0U,
                },
                state.Text(node));
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
    state.instance_scratch.assign(state.instances.size(), 0U);
    uint16_t* changed_instances = state.instance_scratch.data();
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
        keyframe ? (wire_generation == UINT32_MAX ? 1U : wire_generation + 1U) : state.generation;
    const uint32_t next_revision = keyframe ? 1U : state.revision + 1U;
    const micropixel_graphics_scene_header_t header{
        .magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC,
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
        status = micropixel_service_submit(graphics_scene_service.service_handle, MICROPIXEL_GRAPHICS_CHANNEL_SCENE,
                                           SceneWriter::scene_wire, writer.size_);
    }
    if (status == MICROPIXEL_STATUS_OK) {
        displayed_scene = &state;
        wire_generation = next_generation;
        state.generation = next_generation;
        state.revision = next_revision;
        state.valid = true;
        state.texture_revision = runtime::texture_revision;
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

bool NodeHandle::valid() const { return StateAlive(state_) && state_->NodeValid(id_, generation_); }

void NodeHandle::Destroy() {
    if (!valid()) {
        return;
    }
    ValidateHandle(state_);
    state_->BeginPending();
    state_->DestroyNode(id_, generation_);
}

bool Container::valid() const {
    return StateAlive(state_) &&
           (id_ == 0U ? generation_ == state_->root_identity : state_->ContainerValid(id_, generation_));
}

Point Container::SceneTranslation() const {
    if (!valid()) {
        runtime::Panic("scene.container.invalid", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    int64_t x = 0;
    int64_t y = 0;
    uint16_t container_id = id_;
    for (uint16_t depth = 0U; container_id != 0U && depth <= state_->container_count; ++depth) {
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

Result<void> ContainerNode::Destroy() {
    if (!valid()) {
        return {};
    }
    state_->BeginPending();
    state_->DestroyContainer(id_, generation_);
    return {};
}

void NodeHandle::SetVisible(bool visible) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.visible == visible) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.visible = visible;
    detail::UpdateScenePropertyDirty(node.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY,
                                     node.visible != original.visible);
}

void ContainerNode::SetClip(Rect clip) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.clip == clip) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.clip = clip;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_CLIP,
                                     container.clip != original.clip);
}

void ContainerNode::SetTranslation(Point translation) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.translation == translation) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.translation = translation;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_TRANSLATION,
                                     container.translation != original.translation);
}

void ContainerNode::SetOpacity(uint8_t opacity) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.opacity == opacity) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.opacity = opacity;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE,
                                     container.opacity != original.opacity || container.visible != original.visible);
}

void ContainerNode::SetVisible(bool visible) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.visible == visible) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.visible = visible;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_APPEARANCE,
                                     container.opacity != original.opacity || container.visible != original.visible);
}

void ContainerNode::SetZOrder(int16_t z_order) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.z_order == z_order) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.z_order = z_order;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_Z_ORDER,
                                     container.z_order != original.z_order);
}

void ContainerNode::SetCacheContent(bool cache_content) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneContainerData& container = state_->Container(id_, generation_);
    if (container.cache_content == cache_content) {
        return;
    }
    const SceneContainerData& original = state_->RememberContainer(id_);
    container.cache_content = cache_content;
    detail::UpdateScenePropertyDirty(container.dirty, original.dirty, MICROPIXEL_GRAPHICS_SCENE_CONTAINER_FLAGS,
                                     container.cache_content != original.cache_content);
}

void ShapeNode::SetRect(Rect rect) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == rect) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = rect;
    UpdateNodeGeometryDirty(node, original);
}

void ShapeNode::SetColor(Color color) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void ShapeNode::SetOpacity(uint8_t opacity) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void RoundedRectNode::SetRect(Rect rect) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == rect) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = rect;
    UpdateNodeGeometryDirty(node, original);
}

void RoundedRectNode::SetFillColor(Color color) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void RoundedRectNode::SetStrokeColor(Color color) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.stroke_color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.stroke_color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void RoundedRectNode::SetRadius(uint32_t radius) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.radius == radius) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.radius = radius;
    UpdateNodeGeometryDirty(node, original);
}

void RoundedRectNode::SetStrokeWidth(uint32_t stroke_width) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.stroke_width == stroke_width) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.stroke_width = stroke_width;
    UpdateNodeGeometryDirty(node, original);
}

void RoundedRectNode::SetOpacity(uint8_t opacity) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SpriteNode::SetDestination(Rect destination) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination == destination) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination = destination;
    UpdateNodeGeometryDirty(node, original);
}

void SpriteNode::SetSource(Rect source) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.source == source) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.source = source;
    UpdateNodeContentDirty(*state_, node, original);
}

void SpriteNode::SetTexture(const Texture& texture) {
    ValidateHandle(state_);
    state_->BeginPending();
    if (!texture.valid()) {
        runtime::Panic("scene.sprite.texture", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.texture_handle == texture.handle_) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.texture_handle = texture.handle_;
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    UpdateNodeContentDirty(*state_, node, original);
}

void SpriteNode::SetOpacity(uint8_t opacity) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void LabelNode::SetPosition(Point position) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.destination.x == position.x && node.destination.y == position.y) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.destination.x = position.x;
    node.destination.y = position.y;
    UpdateNodeGeometryDirty(node, original);
}

void LabelNode::SetText(const char* text) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    const uint16_t length = TextLength(text);
    bool changed = length != node.text_length;
    const char* current = state_->Text(node);
    for (uint16_t index = 0U; !changed && index < length; ++index) {
        changed = current[index] != text[index];
    }
    if (!changed) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    if (!state_->StoreText(node, text, length)) {
        runtime::Panic("scene.text.memory", MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    }
    UpdateNodeContentDirty(*state_, node, original);
}

void LabelNode::SetColor(Color color) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.color == color) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.color = color;
    UpdateNodeAppearanceDirty(node, original);
}

void LabelNode::SetFont(SystemFont font) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    const micropixel_font_handle_t handle = static_cast<micropixel_font_handle_t>(font);
    if (handle == 0U) {
        runtime::Panic("scene.label.font", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    if (node.font_handle == handle) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.font_handle = handle;
    UpdateNodeContentDirty(*state_, node, original);
}

void LabelNode::SetCentered(bool centered) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.centered == centered) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.centered = centered;
    UpdateNodeGeometryDirty(node, original);
}

void SpriteBatch::SetTexture(const Texture& texture) {
    ValidateHandle(state_);
    state_->BeginPending();
    if (!texture.valid()) {
        runtime::Panic("scene.batch.texture", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.texture_handle == texture.handle_) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.texture_handle = texture.handle_;
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    UpdateNodeContentDirty(*state_, node, original);
}

void SpriteBatch::SetOpacity(uint8_t opacity) {
    ValidateHandle(state_);
    state_->BeginPending();
    SceneNodeData& node = state_->Node(id_, generation_);
    if (node.opacity == opacity) {
        return;
    }
    const SceneNodeData& original = state_->RememberNode(id_);
    node.opacity = opacity;
    UpdateNodeAppearanceDirty(node, original);
}

void SpriteBatch::SetInstance(uint16_t instance_id, const SpriteInstance& instance) {
    ValidateHandle(state_);
    state_->BeginPending();
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

void SpriteBatch::SetInstanceVisible(uint16_t instance_id, bool visible) {
    ValidateHandle(state_);
    state_->BeginPending();
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

Scene::Scene(CapabilityToken, const SceneDescriptor& descriptor)
    : Container(nullptr, 0U, 0U), owned_state_(new (std::nothrow) SceneState) {
    if (!owned_state_ || next_handle_identity == UINT32_MAX) return;
    const detail::DisplayTransform& display = detail::CurrentDisplayTransform();
    if (descriptor.logical_width == 0U || descriptor.logical_height == 0U ||
        descriptor.logical_width != display.logical_width || descriptor.logical_height != display.logical_height) {
        runtime::Panic("scene.size", MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    state_ = owned_state_.get();
    if (!state_->Reset(descriptor)) {
        state_ = nullptr;
        owned_state_.reset();
        return;
    }
    generation_ = NextIdentity();
    state_->root_identity = generation_;
    state_->next_live = live_scenes;
    live_scenes = state_;
}

Scene::Scene(Scene&& other) noexcept
    : Container(other.state_, 0U, other.generation_), owned_state_(std::move(other.owned_state_)) {
    other.state_ = nullptr;
}

Scene::~Scene() {
    if (state_ != nullptr) {
        if (displayed_scene == state_) displayed_scene = nullptr;
        for (SceneState** link = &live_scenes; *link != nullptr; link = &(*link)->next_live) {
            if (*link == state_) {
                *link = state_->next_live;
                break;
            }
        }
    }
}

void Scene::SetBackground(Color color) {
    ValidateHandle(state_);
    state_->BeginPending();
    if (state_->background_color == color) {
        return;
    }
    state_->RememberBackground();
    state_->background_color = color;
    state_->background_dirty = state_->background_color != state_->background_undo || state_->background_dirty_undo;
}

Result<ContainerNode> Container::CreateContainer(const ContainerProperties& properties) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    auto allocated = state_->AllocateContainer(*this, properties);
    if (!allocated) return unexpected(allocated.error());
    const uint16_t id = allocated.value();
    return ContainerNode{state_, id, state_->containers[id].generation};
}

Result<ShapeNode> Container::CreateShape(Rect rect, Color color, uint8_t opacity) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    auto allocated = state_->AllocateNode(*this);
    if (!allocated) return unexpected(allocated.error());
    const uint16_t id = allocated.value();
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kShape;
    node.dirty = FullMask(SceneNodeKind::kShape);
    node.visible = true;
    node.destination = rect;
    node.color = color;
    node.opacity = opacity;
    return ShapeNode{state_, id, node.generation};
}

Result<RoundedRectNode> Container::CreateRoundedRect(Rect rect, const RoundedRectStyle& style) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    auto allocated = state_->AllocateNode(*this);
    if (!allocated) return unexpected(allocated.error());
    const uint16_t id = allocated.value();
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

Result<SpriteNode> Container::CreateSprite(const Texture& texture, Rect destination, Rect source, uint8_t opacity) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    if (!texture.valid()) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    auto allocated = state_->AllocateNode(*this);
    if (!allocated) return unexpected(allocated.error());
    const uint16_t id = allocated.value();
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kSprite;
    node.dirty = FullMask(SceneNodeKind::kSprite);
    node.visible = true;
    node.destination = destination;
    node.color = Color::Black();
    node.opacity = opacity;
    node.texture_handle = texture.handle_;
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    node.source = source;
    return SpriteNode{state_, id, node.generation};
}

Result<SpriteBatch> Container::CreateSpriteBatch(const Texture& texture, uint16_t capacity, uint8_t opacity) {
    if (!texture.valid()) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    auto created = CreateSpriteBatchInternal(texture.handle_, capacity, opacity);
    if (!created) return unexpected(created.error());
    auto batch = created.value();
    SceneNodeData& node = state_->Node(batch.id_, batch.generation_);
    node.texture_logical_width = texture.width_;
    node.texture_logical_height = texture.height_;
    node.texture_physical_width = texture.physical_width_;
    node.texture_physical_height = texture.physical_height_;
    return batch;
}

Result<SpriteBatch> Container::CreateSpriteBatch(uint16_t capacity, uint8_t opacity) {
    return CreateSpriteBatchInternal(0U, capacity, opacity);
}

Result<SpriteBatch> Container::CreateSpriteBatchInternal(uint32_t texture_handle, uint16_t capacity, uint8_t opacity) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    if (capacity == 0U) return unexpected(Error{ErrorCode::kInvalidArgument});
    // One node plus `capacity` instances in the shared uint16 item space.
    if (!state_->ItemsAvailable(static_cast<uint32_t>(capacity) + 1U)) {
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }
    const size_t required = static_cast<size_t>(state_->batch_instance_count) + capacity;
    if (!state_->instances.Reserve(required) || !state_->instance_undo_slot.Reserve(required) ||
        !state_->instance_undo.Reserve(required) || !state_->instance_scratch.Reserve(required))
        return unexpected(Error{ErrorCode::kResourceExhausted});
    auto allocated = state_->AllocateNode(*this);
    if (!allocated) return unexpected(allocated.error());
    const uint16_t id = allocated.value();
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
    node.texture_handle = texture_handle;
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

Result<LabelNode> Container::CreateLabel(Point position, const char* text, Color color, SystemFont font,
                                         bool centered) {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    if (text == nullptr || font < SystemFont::kSmall || font > SystemFont::kTitle)
        return unexpected(Error{ErrorCode::kInvalidArgument});
    const uint32_t max_text_bytes = runtime::LoadGraphicsLimits().max_text_bytes;
    uint16_t length = 0;
    while (length <= max_text_bytes && text[length] != '\0') ++length;
    if (length > max_text_bytes) return unexpected(Error{ErrorCode::kInvalidArgument});
    auto allocated = state_->AllocateNode(*this);
    if (!allocated) return unexpected(allocated.error());
    const uint16_t id = allocated.value();
    SceneNodeData& node = state_->nodes[id];
    node.kind = SceneNodeKind::kLabel;
    node.dirty = FullMask(SceneNodeKind::kLabel);
    node.visible = true;
    node.centered = centered;
    node.destination = {.x = position.x, .y = position.y};
    node.color = color;
    node.opacity = 255U;
    node.font_handle = static_cast<micropixel_font_handle_t>(font);
    if (!state_->StoreText(node, text, length)) {
        state_->DestroyNode(id, node.generation);
        return unexpected(Error{ErrorCode::kResourceExhausted});
    }
    return LabelNode{state_, id, node.generation};
}

uint16_t Scene::node_count() const {
    ValidateHandle(state_);
    return state_->node_count;
}

Result<void> Renderer::Present(const Scene& scene) const {
    ValidateHandle(scene.state_);
    auto& state = *scene.state_;
    state.BeginPending();
    int32_t status = EncodeAndSubmit(state, !state.valid || state.revision == UINT32_MAX);
    if (status == MICROPIXEL_STATUS_STALE_STATE) {
        state.valid = false;
        status = EncodeAndSubmit(state, true);
    }
    if (status == MICROPIXEL_STATUS_OK) state.AcceptFrame();
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(StatusError(status))};
}

Result<Scene> Renderer::CreateScene(Color background) const {
    const detail::DisplayTransform& display = detail::CurrentDisplayTransform();
    return CreateScene(
        {.logical_width = display.logical_width, .logical_height = display.logical_height, .background = background});
}

Result<Scene> Renderer::CreateScene(const SceneDescriptor& descriptor) const {
    const auto& display = detail::CurrentDisplayTransform();
    auto resolved = descriptor;
    if (resolved.logical_width == 0) resolved.logical_width = display.logical_width;
    if (resolved.logical_height == 0) resolved.logical_height = display.logical_height;
    if (resolved.logical_width != display.logical_width || resolved.logical_height != display.logical_height) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    Scene scene{Scene::CapabilityToken{}, resolved};
    if (!scene.valid()) return unexpected(Error{ErrorCode::kResourceExhausted});
    return scene;
}

}  // namespace micropixel
