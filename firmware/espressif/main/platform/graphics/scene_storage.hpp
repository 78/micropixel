#pragma once

#include <algorithm>
#include <cstring>
#include <expected>
#include <utility>

#include "device/contracts/graphics.hpp"
#include "platform/graphics/app_surface_compositor.hpp"
#include "platform/memory/psram_buffer.hpp"

namespace micropixel::platform::graphics {

struct SceneCapacity final {
    uint16_t nodes{};
    uint16_t instances{};
    uint16_t containers{};
    // Text bytes this message may append (text_length + NUL per TEXT record).
    uint32_t text_bytes{};
};

// Scene ids are uint16 on the wire; every node and every batch instance is at
// most one draw operation and operation indices are uint16 as well.
inline constexpr size_t kMaxSceneItems = UINT16_MAX;
inline constexpr size_t kMaxDrawOperations = UINT16_MAX;
// Live text of one scene (every node at most kMaxTextBytes); this only stops
// the doubling growth from overflowing, PSRAM runs out long before.
inline constexpr size_t kMaxTextArenaBytes = size_t{1} << 24U;

// Validate the untrusted envelope and record boundaries before allocating.
// Semantic checks and revision validation still belong to GuestScene::Apply.
inline std::expected<SceneCapacity, int32_t> ReadSceneCapacity(const uint8_t* bytes, uint32_t length) {
    micropixel_graphics_scene_header_t header{};
    if (bytes == nullptr || length < sizeof(header) || length > micropixel::device::graphics_limits::kMaxSceneBytes) {
        return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    std::memcpy(&header, bytes, sizeof(header));
    if (header.magic != MICROPIXEL_GRAPHICS_SCENE_MAGIC || header.total_size != length || header.flags != 0U ||
        (header.kind != MICROPIXEL_GRAPHICS_SCENE_KEYFRAME && header.kind != MICROPIXEL_GRAPHICS_SCENE_PATCH) ||
        static_cast<size_t>(header.node_count) + header.batch_instance_count > kMaxDrawOperations ||
        header.container_count == UINT16_MAX) {
        return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    uint32_t text_bytes = 0U;
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
        if (record.opcode == MICROPIXEL_GRAPHICS_SCENE_OP_TEXT) {
            micropixel_graphics_scene_text_record_t text{};
            if (record.size < sizeof(text)) {
                return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
            }
            std::memcpy(&text, bytes + offset, sizeof(text));
            text_bytes += static_cast<uint32_t>(text.text_length) + 1U;
        }
        offset += record.size;
    }
    if (offset != length) {
        return std::unexpected(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    return SceneCapacity{header.node_count, header.batch_instance_count, header.container_count, text_bytes};
}

// One App's working set. Ensure stages ALL allocations, then migrates both
// consumers before releasing old memory. No allocator call occurs on reuse.
// Node, container and instance counts have no policy cap: they grow until the
// uint16 wire ids or PSRAM run out.
class SceneStorage final {
   public:
    // Makes room for `required` on top of the committed scene. Text is the only
    // resource whose need depends on the current scene: the message's bytes are
    // appended behind whatever the arena already holds, so a fragmented arena is
    // compacted first, and the arena only grows when live text plus the message
    // do not fit. A rebind drops the compositor's retained operations (their text
    // pointers die with the old arena); a compaction only forces a full
    // re-normalize because the old half stays readable for one more compose.
    [[nodiscard]] bool Ensure(SceneCapacity required, GuestScene* scene, AppSurfaceCompositor* compositor) {
        if (required.nodes > kMaxSceneItems || required.instances > kMaxSceneItems ||
            required.containers >= kMaxSceneItems ||
            static_cast<size_t>(required.nodes) + required.instances > kMaxDrawOperations) {
            return false;
        }
        const size_t nodes = Growth(node_capacity_, required.nodes, kMaxSceneItems);
        const size_t instances = Growth(instance_capacity_, required.instances, kMaxSceneItems);
        const size_t containers = Growth(container_capacity_, required.containers, kMaxSceneItems - 1U);
        const size_t operations =
            Growth(operation_capacity_, static_cast<size_t>(required.nodes) + required.instances, kMaxDrawOperations);
        size_t text_bytes = text_capacity_;
        if (scene != nullptr) {
            const uint64_t appended = static_cast<uint64_t>(scene->TextUsed()) + required.text_bytes;
            if (appended > text_capacity_) {
                const uint64_t live = static_cast<uint64_t>(scene->TextLive()) + required.text_bytes;
                if (live <= text_capacity_) {
                    // Compaction moves live text to the other arena half. The
                    // retained operations still read the old half, so they may
                    // be compared once more but must all be rebuilt this frame.
                    scene->CompactText();
                    if (compositor != nullptr) {
                        compositor->InvalidateRetainedText();
                    }
                } else if (live > kMaxTextArenaBytes) {
                    return false;
                } else {
                    text_bytes = Growth(text_capacity_, static_cast<size_t>(live), kMaxTextArenaBytes);
                }
            }
        } else {
            text_bytes = Growth(text_capacity_, required.text_bytes, kMaxTextArenaBytes);
        }
        if (nodes == node_capacity_ && instances == instance_capacity_ && containers == container_capacity_ &&
            operations == operation_capacity_ && text_bytes == text_capacity_) {
            return true;
        }
        SceneStorage replacement;
        if (!replacement.nodes_.Allocate(2U * nodes) || !replacement.instances_.Allocate(2U * instances) ||
            !replacement.containers_.Allocate(2U * (containers + 1U)) ||
            !replacement.draw_order_.Allocate(2U * nodes) || !replacement.node_changes_.Allocate(nodes) ||
            !replacement.instance_changes_.Allocate(instances) ||
            !replacement.container_changes_.Allocate(containers + 1U) || !replacement.node_marks_.Allocate(nodes) ||
            !replacement.instance_marks_.Allocate(instances) ||
            !replacement.container_marks_.Allocate(containers + 1U) || !replacement.text_.Allocate(2U * text_bytes) ||
            !replacement.operations_.Allocate(2U * operations) || !replacement.stale_indices_.Allocate(operations) ||
            !replacement.sorted_indices_.Allocate(operations) ||
            !replacement.container_path_.Allocate(containers + 1U)) {
            return false;
        }
        replacement.node_capacity_ = nodes;
        replacement.instance_capacity_ = instances;
        replacement.container_capacity_ = containers;
        replacement.operation_capacity_ = operations;
        replacement.text_capacity_ = text_bytes;
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
        return {nodes_.View(),
                instances_.View(),
                containers_.View(),
                draw_order_.View(),
                node_changes_.View(),
                instance_changes_.View(),
                container_changes_.View(),
                node_marks_.View(),
                instance_marks_.View(),
                container_marks_.View(),
                text_.View()};
    }
    [[nodiscard]] AppSurfaceStorageView SurfaceView() const {
        return {operations_.View(), stale_indices_.View(), sorted_indices_.View(), container_path_.View()};
    }
    [[nodiscard]] size_t OperationCapacity() const { return operation_capacity_; }
    [[nodiscard]] size_t NodeCapacity() const { return node_capacity_; }
    [[nodiscard]] size_t InstanceCapacity() const { return instance_capacity_; }
    [[nodiscard]] size_t ContainerCapacity() const { return container_capacity_; }
    [[nodiscard]] size_t TextCapacity() const { return text_capacity_; }

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
    memory::PsramBuffer<uint8_t> container_changes_;
    memory::PsramBuffer<uint8_t> node_marks_;
    memory::PsramBuffer<uint8_t> instance_marks_;
    memory::PsramBuffer<uint8_t> container_marks_;
    memory::PsramBuffer<char> text_;
    memory::PsramBuffer<AppDrawOperation> operations_;
    memory::PsramBuffer<uint16_t> stale_indices_;
    memory::PsramBuffer<uint16_t> sorted_indices_;
    memory::PsramBuffer<uint16_t> container_path_;
    size_t node_capacity_{};
    size_t instance_capacity_{};
    size_t container_capacity_{};
    size_t operation_capacity_{};
    size_t text_capacity_{};
};

}  // namespace micropixel::platform::graphics
