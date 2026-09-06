#pragma once

#include <algorithm>
#include <cstring>
#include <expected>
#include <utility>

#include "platform/graphics/app_surface_compositor.hpp"
#include "platform/memory/psram_buffer.hpp"

namespace micropixel::platform::graphics {

struct SceneCapacity final {
    uint16_t nodes{};
    uint16_t instances{};
};

// Validate the untrusted envelope and record boundaries before allocating.
// Semantic checks and revision validation still belong to GuestScene::Apply.
inline std::expected<SceneCapacity, int32_t> ReadSceneCapacity(const uint8_t* bytes, uint32_t length) {
    micropixel_graphics_scene_header_t header{};
    if (bytes == nullptr || length < sizeof(header) || length > MICROPIXEL_GRAPHICS_MAX_SCENE_BYTES) {
        return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != MICROPIXEL_GRAPHICS_SCENE_MAGIC || header.total_size != length || header.flags != 0U ||
        header.interface_major != MICROPIXEL_GRAPHICS_INTERFACE_MAJOR ||
        header.interface_minor > MICROPIXEL_GRAPHICS_INTERFACE_MINOR ||
        (header.kind != MICROPIXEL_GRAPHICS_SCENE_KEYFRAME && header.kind != MICROPIXEL_GRAPHICS_SCENE_PATCH) ||
        header.node_count > MICROPIXEL_GRAPHICS_MAX_SCENE_NODES ||
        header.batch_instance_count > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES ||
        header.container_count > MICROPIXEL_GRAPHICS_MAX_CONTAINERS) {
        return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    uint32_t offset = sizeof(header);
    for (uint32_t index = 0U; index < header.record_count; ++index) {
        micropixel_graphics_scene_record_header_t record{};
        if (length - offset < sizeof(record)) {
            return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        std::memcpy(&record, bytes + offset, sizeof(record));
        if (record.size < sizeof(record) || (record.size & 3U) != 0U || record.size > length - offset) {
            return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
        }
        offset += record.size;
    }
    if (offset != length) {
        return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return SceneCapacity{header.node_count, header.batch_instance_count};
}

// One App's working set. Grow stages ALL allocations, then migrates both
// consumers before releasing old memory. No allocator call occurs on reuse.
class SceneStorage final {
   public:
    [[nodiscard]] bool Grow(SceneCapacity required, GuestScene* scene, AppSurfaceCompositor* compositor) {
        if (required.nodes > MICROPIXEL_GRAPHICS_MAX_SCENE_NODES ||
            required.instances > MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES) {
            return false;
        }
        const size_t nodes = Growth(node_capacity_, required.nodes, MICROPIXEL_GRAPHICS_MAX_SCENE_NODES);
        const size_t instances =
            Growth(instance_capacity_, required.instances, MICROPIXEL_GRAPHICS_MAX_BATCH_INSTANCES);
        const size_t operations = Growth(operation_capacity_, static_cast<size_t>(required.nodes) + required.instances,
                                         MICROPIXEL_GRAPHICS_MAX_DRAW_OPERATIONS);
        if (nodes == node_capacity_ && instances == instance_capacity_ && operations == operation_capacity_) {
            return true;
        }
        SceneStorage replacement;
        if (!replacement.nodes_.Allocate(2U * nodes) || !replacement.instances_.Allocate(2U * instances) ||
            !replacement.containers_.Allocate(2U * (MICROPIXEL_GRAPHICS_MAX_CONTAINERS + 1U)) ||
            !replacement.draw_order_.Allocate(2U * nodes) || !replacement.node_changes_.Allocate(nodes) ||
            !replacement.instance_changes_.Allocate(instances) || !replacement.operations_.Allocate(2U * operations) ||
            !replacement.stale_indices_.Allocate(operations) || !replacement.sorted_indices_.Allocate(operations)) {
            return false;
        }
        replacement.node_capacity_ = nodes;
        replacement.instance_capacity_ = instances;
        replacement.operation_capacity_ = operations;
        if (scene != nullptr) {
            scene->RebindStorage(replacement.SceneView());
        }
        if (compositor != nullptr) {
            compositor->RebindStorage(replacement.SurfaceView());
        }
        *this = std::move(replacement);
        return true;
    }
    void Reset() { *this = SceneStorage{}; }
    [[nodiscard]] GuestSceneStorageView SceneView() const {
        return {nodes_.View(),      instances_.View(),    containers_.View(),
                draw_order_.View(), node_changes_.View(), instance_changes_.View()};
    }
    [[nodiscard]] AppSurfaceStorageView SurfaceView() const {
        return {operations_.View(), stale_indices_.View(), sorted_indices_.View()};
    }
    [[nodiscard]] size_t OperationCapacity() const { return operation_capacity_; }
    [[nodiscard]] size_t NodeCapacity() const { return node_capacity_; }
    [[nodiscard]] size_t InstanceCapacity() const { return instance_capacity_; }

   private:
    static size_t Growth(size_t current, size_t required, size_t limit) {
        size_t capacity = std::max<size_t>(current, 16U);
        while (capacity < required) {
            capacity = std::min(limit, capacity * 2U);
        }
        return capacity;
    }
    memory::PsramBuffer<GuestSceneNode> nodes_;
    memory::PsramBuffer<GuestSceneSpriteInstance> instances_;
    memory::PsramBuffer<GuestSceneContainer> containers_;
    memory::PsramBuffer<uint16_t> draw_order_;
    memory::PsramBuffer<uint8_t> node_changes_;
    memory::PsramBuffer<uint8_t> instance_changes_;
    memory::PsramBuffer<AppDrawOperation> operations_;
    memory::PsramBuffer<uint16_t> stale_indices_;
    memory::PsramBuffer<uint16_t> sorted_indices_;
    size_t node_capacity_{};
    size_t instance_capacity_{};
    size_t operation_capacity_{};
};

}  // namespace micropixel::platform::graphics
