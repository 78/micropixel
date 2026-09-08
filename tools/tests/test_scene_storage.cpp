#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "platform/graphics/scene_storage.hpp"

namespace graphics = micropixel::platform::graphics;
namespace {
size_t attempts{};
size_t fail_at{};
std::unordered_set<void*> live;
}  // namespace

void* micropixel_test_psram_allocate(size_t size) {
    if (++attempts == fail_at) {
        return nullptr;
    }
    void* pointer = std::malloc(size);
    assert(pointer != nullptr);
    live.insert(pointer);
    return pointer;
}
void micropixel_test_psram_free(void* pointer) {
    if (pointer != nullptr) {
        assert(live.erase(pointer) == 1U);
        std::free(pointer);
    }
}

namespace {
constexpr uint32_t kNodeMask = MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                               MICROPIXEL_GRAPHICS_SCENE_NODE_KIND;
constexpr uint32_t kInstanceMask =
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_GEOMETRY | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_CONTENT |
    MICROPIXEL_GRAPHICS_SCENE_INSTANCE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBILITY;

struct Message final {
    micropixel_graphics_scene_header_t header{
        .magic = MICROPIXEL_GRAPHICS_SCENE_MAGIC,
        .kind = MICROPIXEL_GRAPHICS_SCENE_KEYFRAME,
        .generation = 1U,
        .revision = 1U,
    };
    std::vector<uint8_t> bytes = std::vector<uint8_t>(sizeof(header));
    template <typename T>
    void Append(const T& value) {
        const auto* begin = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), begin, begin + sizeof(value));
    }
    template <typename T>
    void Record(const T& value) {
        Append(value);
        ++header.record_count;
    }
    void Instance(uint16_t first, uint16_t count, uint32_t color) {
        Record(micropixel_graphics_scene_batch_instances_record_t{
            .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BATCH_INSTANCES,
                       .size = static_cast<uint16_t>(sizeof(micropixel_graphics_scene_batch_instances_record_t) +
                                                     count * sizeof(micropixel_graphics_scene_sprite_instance_t))},
            .batch_node_id = 0U,
            .first_instance = first,
            .instance_count = count,
            .property_mask = kInstanceMask});
        for (uint16_t index = 0U; index < count; ++index) {
            Append(micropixel_graphics_scene_sprite_instance_t{.x = static_cast<int32_t>((first + index) % 32U),
                                                               .y = static_cast<int32_t>((first + index) / 32U),
                                                               .width = 1,
                                                               .height = 1,
                                                               .rgb888 = color,
                                                               .opacity = 255U,
                                                               .flags = MICROPIXEL_GRAPHICS_SCENE_INSTANCE_VISIBLE});
        }
    }
    void Text(uint16_t node_id, uint32_t mask, std::string_view text) {
        const uint32_t size = (sizeof(micropixel_graphics_scene_text_record_t) + text.size() + 3U) & ~3U;
        micropixel_graphics_scene_text_record_t record{
            .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_TEXT, .size = static_cast<uint16_t>(size)},
                     .node_id = node_id,
                     .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                     .property_mask = mask},
            .x = 1,
            .y = 1,
            .rgb888 = 0xffffffU,
            .font_handle = 1U,
            .text_length = static_cast<uint16_t>(text.size()),
        };
        const size_t offset = bytes.size();
        bytes.resize(offset + size, 0U);
        std::memcpy(bytes.data() + offset, &record, sizeof(record));
        std::memcpy(bytes.data() + offset + sizeof(record), text.data(), text.size());
        ++header.record_count;
    }
    void Finish() {
        if (header.kind == MICROPIXEL_GRAPHICS_SCENE_KEYFRAME) {
            for (uint16_t node = 0U; node < header.node_count; ++node) {
                Record(micropixel_graphics_scene_node_link_record_t{
                    .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_NODE_LINK,
                               .size = sizeof(micropixel_graphics_scene_node_link_record_t)},
                    .node_id = node,
                    .parent_container_id = 0U,
                    .sibling_order = node});
            }
        }
        header.total_size = static_cast<uint32_t>(bytes.size());
        std::memcpy(bytes.data(), &header, sizeof(header));
    }
};
// Batch node 0 plus rect nodes; the caller finishes the message.
Message KeyframeBody(uint16_t instances, uint16_t nodes, uint32_t generation) {
    Message message;
    message.header.node_count = nodes;
    message.header.batch_instance_count = instances;
    message.header.generation = generation;
    message.Record(micropixel_graphics_scene_background_record_t{
        .record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_BACKGROUND,
                   .size = sizeof(micropixel_graphics_scene_background_record_t)},
        .property_mask = MICROPIXEL_GRAPHICS_SCENE_BACKGROUND_COLOR,
        .rgb888 = 0U});
    message.Record(micropixel_graphics_scene_sprite_batch_record_t{
        .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_SPRITE_BATCH,
                            .size = sizeof(micropixel_graphics_scene_sprite_batch_record_t)},
                 .node_id = 0U,
                 .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                 .property_mask = kNodeMask | MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT},
        .capacity = instances,
        .opacity = 255U});
    message.Instance(0U, instances, 0x00ff00U);
    for (uint16_t index = 1U; index < nodes; ++index) {
        message.Record(micropixel_graphics_scene_rect_record_t{
            .node = {.record = {.opcode = MICROPIXEL_GRAPHICS_SCENE_OP_RECT,
                                .size = sizeof(micropixel_graphics_scene_rect_record_t)},
                     .node_id = index,
                     .flags = MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBLE,
                     .property_mask = kNodeMask | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY},
            .x = 40,
            .y = 0,
            .width = 1,
            .height = 1,
            .rgb888 = 0xff0000U,
            .opacity = 255U});
    }
    return message;
}
Message Keyframe(uint16_t instances, uint16_t nodes = 1U, uint32_t generation = 1U) {
    Message message = KeyframeBody(instances, nodes, generation);
    message.Finish();
    return message;
}
Message Patch(uint16_t instances, uint16_t nodes, uint32_t generation, uint32_t revision) {
    Message message;
    message.header.kind = MICROPIXEL_GRAPHICS_SCENE_PATCH;
    message.header.generation = generation;
    message.header.base_revision = revision - 1U;
    message.header.revision = revision;
    message.header.node_count = nodes;
    message.header.batch_instance_count = instances;
    message.Instance(instances - 1U, 1U, 0x0000ffU);
    message.Finish();
    return message;
}
int32_t Apply(graphics::GuestScene& scene, const Message& message) {
    return scene.Apply(message.bytes.data(), message.bytes.size(), 64, 32, nullptr, nullptr, nullptr, nullptr);
}

// Keyframe with `nodes` text nodes of `length` bytes each (node 0 keeps the
// batch so the message shape matches Keyframe()).
Message TextKeyframe(uint16_t nodes, size_t length, uint32_t generation) {
    Message message = KeyframeBody(1U, 1U, generation);
    message.header.node_count = nodes;
    for (uint16_t index = 1U; index < nodes; ++index) {
        message.Text(index,
                     MICROPIXEL_GRAPHICS_SCENE_NODE_KIND | MICROPIXEL_GRAPHICS_SCENE_NODE_GEOMETRY |
                         MICROPIXEL_GRAPHICS_SCENE_NODE_APPEARANCE | MICROPIXEL_GRAPHICS_SCENE_NODE_VISIBILITY |
                         MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT,
                     std::string(length, static_cast<char>('a' + index % 26)));
    }
    message.Finish();
    return message;
}
Message TextPatch(uint16_t nodes, uint16_t node, std::string_view text, uint32_t generation, uint32_t revision) {
    Message message;
    message.header.kind = MICROPIXEL_GRAPHICS_SCENE_PATCH;
    message.header.generation = generation;
    message.header.base_revision = revision - 1U;
    message.header.revision = revision;
    message.header.node_count = nodes;
    message.header.batch_instance_count = 1U;
    message.Text(node, MICROPIXEL_GRAPHICS_SCENE_NODE_CONTENT, text);
    message.Finish();
    return message;
}
bool ValidFont(void*, micropixel_font_handle_t font) { return font == 1U; }
int32_t ApplyText(graphics::GuestScene& scene, const Message& message) {
    return scene.Apply(message.bytes.data(), message.bytes.size(), 64, 32, nullptr, nullptr, ValidFont, nullptr);
}
graphics::SceneCapacity Required(const Message& message) {
    const auto capacity = graphics::ReadSceneCapacity(message.bytes.data(), message.bytes.size());
    assert(capacity.has_value());
    return *capacity;
}

void TextArenaGrowsAndCompacts() {
    graphics::SceneStorage storage;
    const auto keyframe = TextKeyframe(3U, 20U, 1U);
    assert(storage.Ensure(Required(keyframe), nullptr, nullptr));
    graphics::GuestScene scene(storage.SceneView());
    // 2 * 21 bytes requested; the arena starts at the doubling floor above it.
    assert(storage.TextCapacity() == 64U && scene.TextCapacity() == 64U);
    assert(ApplyText(scene, keyframe) == MICROPIXEL_STATUS_OK && scene.TextUsed() == 42U);
    const std::string node1(scene.Text(scene.Nodes()[1]));
    assert(node1.size() == 20U && node1[0] == 'b');

    // Patches append; once used + message exceeds the half, Ensure compacts
    // in place (live text fits) without touching the allocator.
    uint32_t revision = 1U;
    size_t allocations = live.size();
    for (int round = 0; round < 6; ++round) {
        const auto patch = TextPatch(3U, 2U, std::string(10U, 'z'), 1U, ++revision);
        assert(storage.Ensure(Required(patch), &scene, nullptr));
        assert(scene.TextUsed() + 11U <= scene.TextCapacity());
        assert(ApplyText(scene, patch) == MICROPIXEL_STATUS_OK);
        assert(std::string_view(scene.Text(scene.Nodes()[2])) == std::string(10U, 'z'));
        assert(std::string_view(scene.Text(scene.Nodes()[1])) == node1);
    }
    assert(live.size() == allocations && storage.TextCapacity() == 64U);

    // Live text that no longer fits grows the arena; committed text survives.
    const auto big = TextPatch(3U, 1U, std::string(60U, 'q'), 1U, ++revision);
    assert(storage.Ensure(Required(big), &scene, nullptr));
    assert(storage.TextCapacity() == 128U && scene.TextCapacity() == 128U && live.size() == allocations);
    assert(std::string_view(scene.Text(scene.Nodes()[1])) == node1);
    assert(ApplyText(scene, big) == MICROPIXEL_STATUS_OK);
    assert(std::string_view(scene.Text(scene.Nodes()[1])) == std::string(60U, 'q'));
    assert(std::string_view(scene.Text(scene.Nodes()[2])) == std::string(10U, 'z'));

    // Node and container counts are not capped by policy: well beyond the old
    // 256 / 64 limits the only bound is memory.
    const auto wide = Keyframe(1200U, 1000U, 2U);
    const auto capacity = graphics::ReadSceneCapacity(wide.bytes.data(), wide.bytes.size());
    assert(capacity.has_value() && capacity->nodes == 1000U && capacity->instances == 1200U);
    assert(storage.Ensure(*capacity, &scene, nullptr));
    assert(storage.NodeCapacity() >= 1000U && storage.InstanceCapacity() >= 1200U);
    assert(Apply(scene, wide) == MICROPIXEL_STATUS_OK && scene.NodeCount() == 1000U);
    scene.Reset();
    storage.Reset();
    assert(live.empty());
}

void GrowthRollbackAndReuse() {
    graphics::SceneStorage storage;
    assert(storage.NodeCapacity() == 0U && live.empty());
    assert(storage.Ensure({.nodes = 1, .instances = 2}, nullptr, nullptr));
    graphics::GuestScene scene(storage.SceneView());
    graphics::SoftwarePixelCompositor pixels;
    graphics::AppSurfaceCompositor compositor(storage.SurfaceView(), pixels, {});
    std::array<uint8_t, 64U * 32U * 3U> image{};
    const graphics::PixelSurface surface{.pixels = image.data(),
                                         .size = image.size(),
                                         .width = 64,
                                         .height = 32,
                                         .stride = 64U * 3U,
                                         .format = graphics::SurfacePixelFormat::kBgr888};
    assert(Apply(scene, Keyframe(2)) == MICROPIXEL_STATUS_OK);
    assert(compositor.PresentScene(scene, surface, nullptr, nullptr).status == graphics::AppSurfaceStatus::kOk);
    const auto original_image = image;
    const auto* old_nodes = scene.Nodes();
    const size_t allocations = live.size();
    // Fail every allocation in the coordinated replacement, not only the first.
    for (size_t failure = 1U; failure <= allocations; ++failure) {
        fail_at = attempts + failure;
        assert(!storage.Ensure({.nodes = 256, .instances = 1024}, &scene, &compositor));
        assert(live.size() == allocations && scene.Nodes() == old_nodes);
        assert(scene.Revision() == 1U && storage.InstanceCapacity() == 16U);
        assert(compositor.PresentScene(scene, surface, nullptr, nullptr).status == graphics::AppSurfaceStatus::kOk);
        assert(image == original_image);
    }
    fail_at = 0U;
    assert(storage.Ensure({.nodes = 256, .instances = 1024}, &scene, &compositor));
    assert(scene.Nodes() != old_nodes && scene.Revision() == 1U);
    // A patch based on the old committed scene must remain valid after rebinding.
    assert(Apply(scene, Patch(2, 1, 1, 2)) == MICROPIXEL_STATUS_OK);
    assert(compositor.PresentScene(scene, surface, nullptr, nullptr).status == graphics::AppSurfaceStatus::kOk);
    assert(image[3] == 255U && image[4] == 0U && image[5] == 0U);
    const auto large = Keyframe(1024, 256, 2);
    assert(graphics::ReadSceneCapacity(large.bytes.data(), large.bytes.size()).has_value());
    assert(Apply(scene, large) == MICROPIXEL_STATUS_OK);
    assert(compositor.PresentScene(scene, surface, nullptr, nullptr).status == graphics::AppSurfaceStatus::kOk);
    assert(compositor.CurrentOperationCount() == 1279U);
    assert(Apply(scene, Patch(1024, 256, 2, 2)) == MICROPIXEL_STATUS_OK);
    assert(compositor.PresentScene(scene, surface, nullptr, nullptr).status == graphics::AppSurfaceStatus::kOk);
    const size_t last = (31U * 64U + 31U) * 3U;
    assert(image[last] == 255U && image[last + 1U] == 0U);
    const size_t before = attempts;
    assert(storage.Ensure({.nodes = 1, .instances = 1}, &scene, &compositor));
    assert(attempts == before);  // No allocation, shrink or pointer movement.
    const auto preserved = image;
    auto invalid = large;
    invalid.bytes.back() = 0xffU;  // Nonzero reserved padding in the last node.
    assert(Apply(scene, invalid) != MICROPIXEL_STATUS_OK);
    assert(scene.Revision() == 2U && image == preserved);
    compositor.Reset();
    scene.Reset();
    storage.Reset();
    assert(live.empty());
}

void RejectEnvelopeBeforeAllocation() {
    auto message = Keyframe(2);
    const size_t before = attempts;
    // Counts have no policy cap, only the uint16 operation index space.
    message.header.batch_instance_count = 40000U;
    message.header.node_count = 30000U;
    message.Finish();
    assert(!graphics::ReadSceneCapacity(message.bytes.data(), message.bytes.size()));
    message.header.batch_instance_count = 2U;
    message.header.node_count = 1U;
    message.header.container_count = UINT16_MAX;
    message.Finish();
    assert(!graphics::ReadSceneCapacity(message.bytes.data(), message.bytes.size()));
    message.header.container_count = 0U;
    message.header.node_count = 1U;
    message.Finish();
    message.bytes.pop_back();
    assert(!graphics::ReadSceneCapacity(message.bytes.data(), message.bytes.size()));
    message = Keyframe(2);
    auto* record = reinterpret_cast<micropixel_graphics_scene_record_header_t*>(
        message.bytes.data() + sizeof(micropixel_graphics_scene_header_t));
    record->size = 0U;
    assert(!graphics::ReadSceneCapacity(message.bytes.data(), message.bytes.size()));
    assert(attempts == before && live.empty());
}
}  // namespace

int main() {
    GrowthRollbackAndReuse();
    TextArenaGrowsAndCompacts();
    RejectEnvelopeBeforeAllocation();
    std::cout << "scene storage: OOM rollback, growth, text arena, uncapped counts, patches and release passed\n";
}
